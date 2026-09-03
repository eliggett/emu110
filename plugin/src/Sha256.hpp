// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    A small SHA-256, for identifying ROM images in a saved session.

    This is IDENTITY, not security: it answers "is this the same file the session was made
    with?" and nothing else.  It is here rather than pulled in from a library because the
    plugin has no other reason to depend on one, and because a hash that ships in the same
    binary can never disagree with itself across versions.

    FIPS 180-4.  Verified against the standard's own two test vectors in plugin/tools.
*/

#ifndef VOLTAIRE_SHA256_HPP
#define VOLTAIRE_SHA256_HPP

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace voltaire {

class Sha256
{
public:
	Sha256() { reset(); }

	void reset()
	{
		m_len = 0;
		m_fill = 0;
		static const uint32_t iv[8] = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
		};
		std::memcpy(m_h, iv, sizeof(m_h));
	}

	void update(const uint8_t *data, size_t n)
	{
		m_len += n;
		while (n)
		{
			const size_t take = (64 - m_fill) < n ? (64 - m_fill) : n;
			std::memcpy(m_block + m_fill, data, take);
			m_fill += take;
			data += take;
			n -= take;
			if (m_fill == 64)
			{
				compress(m_block);
				m_fill = 0;
			}
		}
	}

	/// Lower-case hex, 64 characters.
	std::string hex()
	{
		// Padding: 0x80, zeroes, then the length in BITS as a big-endian 64-bit count.
		const uint64_t bits = m_len * 8;
		uint8_t pad[72];
		size_t padLen = 0;
		pad[padLen ++] = 0x80;
		while ((m_fill + padLen) % 64 != 56)
			pad[padLen ++] = 0;
		for (int i = 7; i >= 0; i --)
			pad[padLen ++] = uint8_t(bits >> (i * 8));
		update(pad, padLen);

		static const char *const digits = "0123456789abcdef";
		std::string out;
		out.reserve(64);
		for (int i = 0; i < 8; i ++)
			for (int b = 3; b >= 0; b --)
			{
				const uint8_t byte = uint8_t(m_h[i] >> (b * 8));
				out.push_back(digits[byte >> 4]);
				out.push_back(digits[byte & 0x0f]);
			}
		return out;
	}

	static std::string of(const uint8_t *data, size_t n)
	{
		Sha256 s;
		s.update(data, n);
		return s.hex();
	}

private:
	static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void compress(const uint8_t *p)
	{
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
			0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
			0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
			0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
			0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
			0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
			0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
			0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
			0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};

		uint32_t w[64];
		for (int i = 0; i < 16; i ++)
			w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16)
			     | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
		for (int i = 16; i < 64; i ++)
		{
			const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3];
		uint32_t e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
		for (int i = 0; i < 64; i ++)
		{
			const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
			const uint32_t ch = (e & f) ^ (~e & g);
			const uint32_t t1 = h + S1 + ch + k[i] + w[i];
			const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = S0 + maj;
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		m_h[0] += a; m_h[1] += b; m_h[2] += c; m_h[3] += d;
		m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += h;
	}

	uint32_t m_h[8];
	uint64_t m_len = 0;
	uint8_t  m_block[64];
	size_t   m_fill = 0;
};

} // namespace voltaire

#endif // VOLTAIRE_SHA256_HPP
