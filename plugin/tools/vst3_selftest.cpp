// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    A VST3 host, cut down to the one question worth asking automatically: does anything the
    DSP pushes ever reach the UI?

    It is the sibling of tools/clap_selftest.c and exists for the same reason, except that
    VST3 was worse.  CLAP at least had the wiring: a updateState() that took the value,
    dropped it and answered true.  VST3 had no wiring at all -- DistrhoPluginVST3.cpp passed
    a nullptr where every other backend passes a callback, so updateStateValue() answered
    false for the whole life of the plugin.  The panel was drawn and then never updated,
    which looks exactly like a plugin that cannot find its ROMs.

    Writing this was also what found the other half of the bug.  DPF gives VST3 a SEPARATE
    edit controller unless DISTRHO_PLUGIN_WANT_DIRECT_ACCESS is set, and a separate
    controller constructs a second Plugin of its own.  For most plugins that is a few
    floats; here it is a whole second emulated machine, booted, never heard, and -- the part
    that matters -- the one whose state the UI was being shown.  See
    patches/0005-vst3-single-component.patch.

    So: load the module, build the two objects a VST3 host builds, connect them the way a
    host connects them, open the editor into a real X window, run audio through the
    processor and pump the timer the view asked the run loop for.  The plugin's own trace
    (VOLTAIRE_TRACE_STATE) then says how many panel updates the UI decoded.

    Deliberately not a general host.  It implements what DPF asks for and nothing else, and
    it trusts the plugin, because the plugin is ours.  In particular the audio thread is
    this thread: single threaded on purpose, so that a queue which is only safe by accident
    still has to survive being drained between process() calls.
*/

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <X11/Xlib.h>

#include "travesty/base.h"
#include "travesty/factory.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/audio_processor.h"
#include "travesty/host.h"
#include "travesty/message.h"
#include "travesty/view.h"
#include "travesty/events.h"

#define FRAMES 256

// ---- refcounting for the objects the plugin holds a reference to ----------------------
//
// Messages really are created and destroyed per push, so they are the one place the count
// has to be honest: the plugin unrefs every message it sends, and leaking one per panel
// update would be thousands over a run.  Everything else here is a singleton the host owns
// for the whole run, so its ref/unref are counters that nobody reads.

template <class T> static uint32_t V3_API single_ref(void*) { return 1; }
template <class T> static uint32_t V3_API single_unref(void*) { return 1; }

// ---- attribute list ------------------------------------------------------------------

struct HostAttrList : v3_attribute_list_cpp
{
    struct Value { int kind = 0; int64_t i = 0; double f = 0.0; std::vector<int16_t> s; };
    std::map<std::string, Value> values;
    int refcount = 1;

    HostAttrList()
    {
        query_interface = qi;
        ref = addref;
        unref = release;
        attrlist.set_int = setInt;
        attrlist.get_int = getInt;
        attrlist.set_float = setFloat;
        attrlist.get_float = getFloat;
        attrlist.set_string = setString;
        attrlist.get_string = getString;
        attrlist.set_binary = setBinary;
        attrlist.get_binary = getBinary;
    }

    static HostAttrList* self(void* p) { return *static_cast<HostAttrList**>(p); }

    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_attribute_list_iid))
        { ++self(p)->refcount; *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static uint32_t V3_API addref(void* p) { return ++self(p)->refcount; }
    static uint32_t V3_API release(void* p) { return --self(p)->refcount; }

    static v3_result V3_API setInt(void* p, const char* id, int64_t v)
    { Value &e = self(p)->values[id]; e.kind = 1; e.i = v; return V3_OK; }
    static v3_result V3_API getInt(void* p, const char* id, int64_t* v)
    {
        auto it = self(p)->values.find(id);
        if (it == self(p)->values.end() || it->second.kind != 1) return V3_FALSE;
        *v = it->second.i; return V3_OK;
    }
    static v3_result V3_API setFloat(void* p, const char* id, double v)
    { Value &e = self(p)->values[id]; e.kind = 2; e.f = v; return V3_OK; }
    static v3_result V3_API getFloat(void* p, const char* id, double* v)
    {
        auto it = self(p)->values.find(id);
        if (it == self(p)->values.end() || it->second.kind != 2) return V3_FALSE;
        *v = it->second.f; return V3_OK;
    }
    static v3_result V3_API setString(void* p, const char* id, const int16_t* s)
    {
        Value &e = self(p)->values[id];
        e.kind = 3; e.s.clear();
        for (const int16_t* q = s; *q != 0; q ++) e.s.push_back(*q);
        e.s.push_back(0);
        return V3_OK;
    }
    static v3_result V3_API getString(void* p, const char* id, int16_t* s, uint32_t size)
    {
        auto it = self(p)->values.find(id);
        if (it == self(p)->values.end() || it->second.kind != 3) return V3_FALSE;
        // size is in BYTES, which is the trap in this interface; the plugin sizes its
        // buffer from the "...:length" int it set alongside, so a short copy here would
        // silently truncate a state value rather than fail.
        const uint32_t room = size / sizeof(int16_t);
        const std::vector<int16_t> &v = it->second.s;
        if (room < v.size()) return V3_FALSE;
        std::memcpy(s, v.data(), v.size() * sizeof(int16_t));
        return V3_OK;
    }
    static v3_result V3_API setBinary(void*, const char*, const void*, uint32_t)
    { return V3_NOT_IMPLEMENTED; }
    static v3_result V3_API getBinary(void*, const char*, const void**, uint32_t*)
    { return V3_NOT_IMPLEMENTED; }
};

// ---- message -------------------------------------------------------------------------

static int g_messagesAlive = 0;

struct HostMessage : v3_message_cpp
{
    std::string id;
    HostAttrList attrs;
    HostAttrList* attrsPtr;
    int refcount = 1;

    HostMessage()
        : attrsPtr(&attrs)
    {
        query_interface = qi;
        ref = addref;
        unref = release;
        msg.get_message_id = getId;
        msg.set_message_id = setId;
        msg.get_attributes = getAttrs;
        ++ g_messagesAlive;
    }
    ~HostMessage() { -- g_messagesAlive; }

    static HostMessage* self(void* p) { return *static_cast<HostMessage**>(p); }

    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_message_iid))
        { ++self(p)->refcount; *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static uint32_t V3_API addref(void* p) { return ++self(p)->refcount; }
    static uint32_t V3_API release(void* p)
    {
        HostMessage* const m = self(p);
        if (-- m->refcount == 0) { delete m; return 0; }
        return m->refcount;
    }

    static const char* V3_API getId(void* p) { return self(p)->id.c_str(); }
    static void V3_API setId(void* p, const char* id) { self(p)->id = id; }
    static v3_attribute_list** V3_API getAttrs(void* p)
    { return (v3_attribute_list**)&self(p)->attrsPtr; }
};

// ---- host application, which is where messages come from ------------------------------

struct HostApp : v3_host_application_cpp
{
    HostApp()
    {
        query_interface = qi;
        ref = single_ref<HostApp>;
        unref = single_unref<HostApp>;
        app.get_name = getName;
        app.create_instance = createInstance;
    }

    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_host_application_iid))
        { *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }

    static v3_result V3_API getName(void*, v3_str_128 name)
    {
        static const char* const n = "Voltaire 110 VST3 self test";
        for (int i = 0; n[i] != '\0'; i ++) name[i] = n[i];
        name[std::strlen(n)] = 0;
        return V3_OK;
    }

    // The one method that has to really work.  sendStateSetToUI() asks for a message here
    // and gives up silently if it does not get one, which would look exactly like the bug
    // this tool exists to detect.
    static v3_result V3_API createInstance(void*, v3_tuid cid, v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(cid, v3_message_iid) || v3_tuid_match(iid, v3_message_iid))
        {
            HostMessage* const m = new HostMessage;
            HostMessage** const mm = new HostMessage*(m);
            *obj = mm;
            return V3_TRUE;
        }
        if (v3_tuid_match(cid, v3_attribute_list_iid) || v3_tuid_match(iid, v3_attribute_list_iid))
        {
            HostAttrList* const a = new HostAttrList;
            HostAttrList** const aa = new HostAttrList*(a);
            *obj = aa;
            return V3_TRUE;
        }
        *obj = nullptr;
        return V3_FALSE;
    }
};

// ---- run loop, which on Linux is how the view gets its idle ---------------------------

struct HostRunLoop : v3_run_loop_cpp
{
    v3_timer_handler** timer = nullptr;
    v3_event_handler** event = nullptr;
    int eventFd = -1;

    HostRunLoop()
    {
        query_interface = qi;
        ref = single_ref<HostRunLoop>;
        unref = single_unref<HostRunLoop>;
        loop.register_event_handler = regEvent;
        loop.unregister_event_handler = unregEvent;
        loop.register_timer = regTimer;
        loop.unregister_timer = unregTimer;
    }

    static HostRunLoop* self(void* p) { return *static_cast<HostRunLoop**>(p); }

    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_run_loop_iid))
        { *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }

    static v3_result V3_API regEvent(void* p, v3_event_handler** h, int fd)
    { self(p)->event = h; self(p)->eventFd = fd; return V3_OK; }
    static v3_result V3_API unregEvent(void* p, v3_event_handler**)
    { self(p)->event = nullptr; return V3_OK; }
    static v3_result V3_API regTimer(void* p, v3_timer_handler** h, uint64_t)
    { self(p)->timer = h; return V3_OK; }
    static v3_result V3_API unregTimer(void* p, v3_timer_handler**)
    { self(p)->timer = nullptr; return V3_OK; }
};

static HostRunLoop  g_runLoop;
static HostRunLoop* g_runLoopPtr = &g_runLoop;

// ---- plugin frame.  Its only real job is to hand over the run loop --------------------

struct HostFrame : v3_plugin_frame_cpp
{
    HostFrame()
    {
        query_interface = qi;
        ref = single_ref<HostFrame>;
        unref = single_unref<HostFrame>;
        frame.resize_view = resizeView;
    }

    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_frame_iid))
        { *iface = p; return V3_OK; }
        if (v3_tuid_match(iid, v3_run_loop_iid))
        { *iface = &g_runLoopPtr; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }

    // A real host resizes the container.  Answering OK without doing it is honest here --
    // there is nothing on screen anybody looks at -- but answering at all matters, since
    // the UI asks on its first idle.
    static v3_result V3_API resizeView(void*, v3_plugin_view**, v3_view_rect*)
    { return V3_OK; }
};

// ---- component handler ----------------------------------------------------------------

struct HostComponentHandler : v3_component_handler_cpp
{
    HostComponentHandler()
    {
        query_interface = qi;
        ref = single_ref<HostComponentHandler>;
        unref = single_unref<HostComponentHandler>;
        comp.begin_edit = beginEdit;
        comp.perform_edit = performEdit;
        comp.end_edit = endEdit;
        comp.restart_component = restartComponent;
    }
    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_component_handler_iid))
        { *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static v3_result V3_API beginEdit(void*, v3_param_id) { return V3_OK; }
    static v3_result V3_API performEdit(void*, v3_param_id, double) { return V3_OK; }
    static v3_result V3_API endEdit(void*, v3_param_id) { return V3_OK; }
    static v3_result V3_API restartComponent(void*, int32_t) { return V3_OK; }
};

// ---- the process data's parameter queues ----------------------------------------------
//
// Output parameter changes are where this plugin's meters live, and DPF will not write
// them at all if the queue is missing -- so they are implemented rather than left null,
// or the run would exercise less of the code than a real host does.

struct HostParamQueue : v3_param_value_queue_cpp
{
    v3_param_id id = 0;
    int points = 0;

    HostParamQueue()
    {
        query_interface = qi;
        ref = single_ref<HostParamQueue>;
        unref = single_unref<HostParamQueue>;
        queue.get_param_id = getId;
        queue.get_point_count = getCount;
        queue.get_point = getPoint;
        queue.add_point = addPoint;
    }
    static HostParamQueue* self(void* p) { return *static_cast<HostParamQueue**>(p); }
    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_value_queue_iid))
        { *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static v3_param_id V3_API getId(void* p) { return self(p)->id; }
    static int32_t V3_API getCount(void* p) { return self(p)->points; }
    static v3_result V3_API getPoint(void*, int32_t, int32_t*, double*) { return V3_FALSE; }
    static v3_result V3_API addPoint(void* p, int32_t, double, int32_t* idx)
    { *idx = self(p)->points ++; return V3_OK; }
};

struct HostParamChanges : v3_param_changes_cpp
{
    static constexpr int kMaxQueues = 64;
    HostParamQueue  pool[kMaxQueues];
    HostParamQueue* poolPtr[kMaxQueues];
    int used = 0;

    HostParamChanges()
    {
        for (int i = 0; i < kMaxQueues; i ++) poolPtr[i] = &pool[i];
        query_interface = qi;
        ref = single_ref<HostParamChanges>;
        unref = single_unref<HostParamChanges>;
        changes.get_param_count = getCount;
        changes.get_param_data = getData;
        changes.add_param_data = addData;
    }
    void reset() { for (int i = 0; i < used; i ++) pool[i].points = 0; used = 0; }

    static HostParamChanges* self(void* p) { return *static_cast<HostParamChanges**>(p); }
    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_changes_iid))
        { *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static int32_t V3_API getCount(void* p) { return self(p)->used; }
    static v3_param_value_queue** V3_API getData(void* p, int32_t idx)
    {
        HostParamChanges* const c = self(p);
        if (idx < 0 || idx >= c->used) return nullptr;
        return (v3_param_value_queue**)&c->poolPtr[idx];
    }
    static v3_param_value_queue** V3_API addData(void* p, const v3_param_id* id, int32_t* idx)
    {
        HostParamChanges* const c = self(p);
        if (c->used >= kMaxQueues) return nullptr;
        const int at = c->used ++;
        c->pool[at].id = *id;
        c->pool[at].points = 0;
        *idx = at;
        return (v3_param_value_queue**)&c->poolPtr[at];
    }
};

// ---- event lists ---------------------------------------------------------------------

struct HostEventList : v3_event_list_cpp
{
    HostEventList()
    {
        query_interface = qi;
        ref = single_ref<HostEventList>;
        unref = single_unref<HostEventList>;
        list.get_event_count = getCount;
        list.get_event = getEvent;
        list.add_event = addEvent;
    }
    static v3_result V3_API qi(void* p, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_event_list_iid))
        { *iface = p; return V3_OK; }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static uint32_t V3_API getCount(void*) { return 0; }
    static v3_result V3_API getEvent(void*, int32_t, v3_event*) { return V3_FALSE; }
    static v3_result V3_API addEvent(void*, v3_event*) { return V3_OK; }
};

// --------------------------------------------------------------------------------------

static HostApp              g_app;              static HostApp*              g_appPtr = &g_app;
static HostFrame            g_frame;            static HostFrame*            g_framePtr = &g_frame;
static HostComponentHandler g_handler;          static HostComponentHandler* g_handlerPtr = &g_handler;
static HostParamChanges     g_inParams;         static HostParamChanges*     g_inParamsPtr = &g_inParams;
static HostParamChanges     g_outParams;        static HostParamChanges*     g_outParamsPtr = &g_outParams;
static HostEventList        g_inEvents;         static HostEventList*        g_inEventsPtr = &g_inEvents;
static HostEventList        g_outEvents;        static HostEventList*        g_outEventsPtr = &g_outEvents;

static void fail(const char* const what)
{
    std::fprintf(stderr, "vst3: %s\n", what);
    std::exit(1);
}

// The module inside a bundle, so the caller can name either.
static std::string modulePath(const char* const given)
{
    std::string p(given);
    while (p.size() > 1 && p[p.size() - 1] == '/') p.erase(p.size() - 1);
    if (p.size() > 5 && p.compare(p.size() - 5, 5, ".vst3") == 0)
    {
        std::string name = p;
        const size_t slash = name.rfind('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        name.erase(name.size() - 5);
        return p + "/Contents/x86_64-linux/" + name + ".so";
    }
    return p;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <plugin.vst3> [blocks]\n", argv[0]);
        return 2;
    }
    const std::string path = modulePath(argv[1]);
    const int blocks = argc > 2 ? std::atoi(argv[2]) : 4000;

    void* const lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr)
    { std::fprintf(stderr, "vst3: cannot load %s: %s\n", path.c_str(), dlerror()); return 1; }

    typedef bool (*ModuleEntryFn)(void*);
    typedef bool (*ModuleExitFn)(void);
    typedef v3_plugin_factory** (*GetFactoryFn)(void);

    const ModuleEntryFn moduleEntry = (ModuleEntryFn)dlsym(lib, "ModuleEntry");
    const ModuleExitFn  moduleExit  = (ModuleExitFn)dlsym(lib, "ModuleExit");
    const GetFactoryFn  getFactory  = (GetFactoryFn)dlsym(lib, "GetPluginFactory");
    if (getFactory == nullptr) fail("no GetPluginFactory export");
    if (moduleEntry != nullptr) moduleEntry(lib);

    v3_plugin_factory** const factory = getFactory();
    if (factory == nullptr) fail("GetPluginFactory returned null");

    const int32_t numClasses = v3_cpp_obj(factory)->num_classes(factory);
    std::printf("vst3: factory advertises %d class(es)\n", numClasses);

    // Class 0 is the audio module.  A factory advertising a SECOND class is advertising a
    // separate edit controller, which for this plugin means a second emulated machine --
    // see the note at the top.  Reported rather than refused, so the tool still works on a
    // build without patch 0005 and says what it found.
    v3_class_info classInfo;
    std::memset(&classInfo, 0, sizeof(classInfo));
    if (v3_cpp_obj(factory)->get_class_info(factory, 0, &classInfo) != V3_OK)
        fail("get_class_info(0) failed");
    std::printf("vst3: class 0 is \"%s\" (%s)\n", classInfo.name, classInfo.category);

    // ---- the component ----------------------------------------------------------------
    v3_component** component = nullptr;
    if (v3_cpp_obj(factory)->create_instance(factory, classInfo.class_id, v3_component_iid,
                                             (void**)&component) != V3_TRUE
        || component == nullptr)
        fail("could not create the component");

    if (v3_cpp_obj_initialize(component, (v3_funknown**)&g_appPtr) != V3_OK)
        fail("component initialize failed");

    // ---- the edit controller, however this build provides it --------------------------
    //
    // Two ways, and which one applies is the whole point of patch 0005.  A single-component
    // build answers v3_edit_controller_iid from the component itself, so the controller and
    // the processor are the same object and share one machine.  A separate-controller build
    // refuses that and makes the host create class 1, which constructs its own.
    v3_edit_controller** controller = nullptr;
    bool separateController = false;
    if (v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller) != V3_OK
        || controller == nullptr)
    {
        separateController = true;
        v3_tuid controllerId;
        if (v3_cpp_obj(component)->get_controller_class_id(component, controllerId) != V3_OK)
            fail("no edit controller: neither cast from the component nor a class id");
        if (v3_cpp_obj(factory)->create_instance(factory, controllerId, v3_edit_controller_iid,
                                                 (void**)&controller) != V3_TRUE
            || controller == nullptr)
            fail("could not create the edit controller");
        if (v3_cpp_obj_initialize(controller, (v3_funknown**)&g_appPtr) != V3_OK)
            fail("controller initialize failed");
    }
    std::printf("vst3: edit controller is %s\n",
                separateController ? "SEPARATE (a second plugin instance)"
                                   : "the component itself (one instance)");

    v3_cpp_obj(controller)->set_component_handler(controller, (v3_component_handler**)&g_handlerPtr);

    // ---- connect component and controller, as a host does ----------------------------
    v3_connection_point** compPoint = nullptr;
    v3_connection_point** ctrlPoint = nullptr;
    v3_cpp_obj_query_interface(component, v3_connection_point_iid, &compPoint);
    v3_cpp_obj_query_interface(controller, v3_connection_point_iid, &ctrlPoint);
    if (compPoint != nullptr && ctrlPoint != nullptr)
    {
        v3_cpp_obj(compPoint)->connect(compPoint, ctrlPoint);
        v3_cpp_obj(ctrlPoint)->connect(ctrlPoint, compPoint);
        std::printf("vst3: component and controller connected\n");
    }
    else
        std::printf("vst3: no component/controller link needed -- they are one object\n");

    // ---- the audio processor ---------------------------------------------------------
    v3_audio_processor** processor = nullptr;
    if (v3_cpp_obj_query_interface(component, v3_audio_processor_iid, &processor) != V3_OK
        || processor == nullptr)
        fail("component has no audio processor");

    v3_process_setup setup;
    setup.process_mode = V3_REALTIME;
    setup.symbolic_sample_size = V3_SAMPLE_32;
    setup.max_block_size = FRAMES;
    setup.sample_rate = 48000.0;
    if (v3_cpp_obj(processor)->setup_processing(processor, &setup) != V3_OK)
        fail("setup_processing failed");

    const int32_t outBuses = v3_cpp_obj(component)->get_bus_count(component, V3_AUDIO, V3_OUTPUT);
    const int32_t evInBuses = v3_cpp_obj(component)->get_bus_count(component, V3_EVENT, V3_INPUT);
    std::printf("vst3: %d audio output bus(es), %d event input bus(es)\n", outBuses, evInBuses);
    for (int32_t i = 0; i < outBuses; i ++)
        v3_cpp_obj(component)->activate_bus(component, V3_AUDIO, V3_OUTPUT, i, true);
    for (int32_t i = 0; i < evInBuses; i ++)
        v3_cpp_obj(component)->activate_bus(component, V3_EVENT, V3_INPUT, i, true);

    // ---- the editor, in a real X window ---------------------------------------------
    Display* const display = XOpenDisplay(nullptr);
    if (display == nullptr)
        fail("no X display -- this check needs one, Xvfb is enough");

    v3_plugin_view** const view = v3_cpp_obj(controller)->create_view(controller, "editor");
    if (view == nullptr) fail("create_view returned null");

    if (v3_cpp_obj(view)->is_platform_type_supported(view, V3_VIEW_PLATFORM_TYPE_X11) != V3_OK)
        fail("the view does not support X11");

    v3_view_rect rect;
    std::memset(&rect, 0, sizeof(rect));
    v3_cpp_obj(view)->get_size(view, &rect);
    const unsigned w = rect.right - rect.left > 0 ? unsigned(rect.right - rect.left) : 800;
    const unsigned h = rect.bottom - rect.top > 0 ? unsigned(rect.bottom - rect.top) : 600;

    const Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                              0, 0, w, h, 0, 0, 0);
    XMapWindow(display, parent);
    XFlush(display);

    // set_frame BEFORE attached: the view looks for the run loop through the frame while
    // attaching, and without it the whole idle mechanism never starts.
    v3_cpp_obj(view)->set_frame(view, (v3_plugin_frame**)&g_framePtr);

    // The controller <-> view link is NOT the host's to make, which is worth stating because
    // it looks like it should be: it is the channel this tool measures, and every other
    // connection here is wired from out here.  dpf_edit_controller::create_view() connects
    // the two itself before it returns, and dpf_ui_connection_point::connect() asserts that
    // it has no peer yet -- so a host that helpfully connects them again breaks the thing
    // it was trying to set up.  All that is checked is that the view HAS one.
    v3_connection_point** viewPoint = nullptr;
    if (v3_cpp_obj_query_interface(view, v3_connection_point_iid, &viewPoint) != V3_OK
        || viewPoint == nullptr)
        fail("no connection point on the view -- the DSP -> UI channel cannot exist");
    v3_cpp_obj_unref(viewPoint);
    std::printf("vst3: view carries a connection point, already joined by create_view\n");

    if (v3_cpp_obj(view)->attached(view, (void*)parent, V3_VIEW_PLATFORM_TYPE_X11) != V3_OK)
        fail("attached failed");
    std::printf("vst3: editor attached, %ux%u\n", w, h);

    if (g_runLoop.timer == nullptr)
        fail("the view registered no timer -- nothing would ever idle");

    // ---- run ------------------------------------------------------------------------
    v3_cpp_obj(component)->set_active(component, true);
    v3_cpp_obj(processor)->set_processing(processor, true);

    float bufL[FRAMES], bufR[FRAMES];
    float* chans[2] = { bufL, bufR };
    v3_audio_bus_buffers out;
    std::memset(&out, 0, sizeof(out));
    out.num_channels = 2;
    out.channel_buffers_32 = chans;

    v3_process_data data;
    std::memset(&data, 0, sizeof(data));
    data.process_mode = V3_REALTIME;
    data.symbolic_sample_size = V3_SAMPLE_32;
    data.nframes = FRAMES;
    data.num_output_buses = 1;
    data.outputs = &out;
    data.input_params = (v3_param_changes**)&g_inParamsPtr;
    data.output_params = (v3_param_changes**)&g_outParamsPtr;
    data.input_events = (v3_event_list**)&g_inEventsPtr;
    data.output_events = (v3_event_list**)&g_outEventsPtr;

    // The machine takes a few seconds of EMULATED time to boot before it puts anything on
    // its display, which is a few thousand blocks and not a few thousand milliseconds.
    for (int i = 0; i < blocks; i ++)
    {
        g_outParams.reset();
        v3_cpp_obj(processor)->process(processor, &data);

        if (g_runLoop.timer != nullptr)
            v3_cpp_obj(g_runLoop.timer)->on_timer(g_runLoop.timer);

        // The UI's X events arrive on the fd it handed the run loop.  Without draining
        // them PUGL's queue grows all run and the window never finishes mapping.
        if (g_runLoop.event != nullptr && XPending(display) > 0)
            v3_cpp_obj(g_runLoop.event)->on_fd_is_set(g_runLoop.event, g_runLoop.eventFd);
    }

    v3_cpp_obj(processor)->set_processing(processor, false);
    v3_cpp_obj(component)->set_active(component, false);

    v3_cpp_obj(view)->removed(view);
    v3_cpp_obj_unref(view);

    XDestroyWindow(display, parent);
    XCloseDisplay(display);

    if (compPoint != nullptr && ctrlPoint != nullptr)
    {
        v3_cpp_obj(compPoint)->disconnect(compPoint, ctrlPoint);
        v3_cpp_obj(ctrlPoint)->disconnect(ctrlPoint, compPoint);
    }
    // Every interface queried out of the component holds a reference, and DPF says so on
    // the way out if one is left -- "asked to delete component while audio processor still
    // active".  Harmless in a test that is about to exit, but it is exactly the warning a
    // real host leak would produce, so it is not left in the output to be ignored.
    v3_cpp_obj_unref(processor);
    if (separateController) v3_cpp_obj_terminate(controller);
    v3_cpp_obj_unref(controller);
    v3_cpp_obj_terminate(component);
    v3_cpp_obj_unref(component);
    if (moduleExit != nullptr) moduleExit();

    // A message leaked per push would be thousands over a run, and the plugin is the thing
    // doing the unref, so this is worth saying out loud.
    std::printf("vst3: ran %d blocks, %d host message(s) still alive\n",
                blocks, g_messagesAlive);
    return 0;
}
