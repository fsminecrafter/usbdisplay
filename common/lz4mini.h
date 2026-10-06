// lz4mini.h - tiny dependency-free LZ4 *block format* codec (compatible with liblz4's LZ4_decompress_safe
// / LZ4_compress_default output). Header only. The compressor is a simple greedy one for inputs < 64 KB;
// the decompressor is bounds-checked and safe on hostile input.
#ifndef LZ4MINI_H
#define LZ4MINI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LZ4M_HASH_BITS 12

static inline uint32_t lz4m_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Returns the compressed size, or 0 if it does not fit in `cap` (or n is too large).
static inline size_t lz4m_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
	if (n >= 0xFFFF) return 0;
	uint16_t tab[1 << LZ4M_HASH_BITS];
	memset(tab, 0xFF, sizeof tab);
	size_t ip = 0, anchor = 0, op = 0;
	if (n >= 13) {
		const size_t mflimit = n - 12, mlimit = n - 5;
		while (ip < mflimit) {
			uint32_t seq = lz4m_rd32(src + ip);
			uint32_t h = (seq * 2654435761u) >> (32 - LZ4M_HASH_BITS);
			uint16_t r = tab[h];
			tab[h] = (uint16_t)ip;
			if (r != 0xFFFF && lz4m_rd32(src + r) == seq) {
				size_t ml = 4;
				while (ip + ml < mlimit && src[r + ml] == src[ip + ml]) ml++;
				size_t lit = ip - anchor, m = ml - 4;
				if (op + 1 + lit + lit / 255 + 1 + 2 + m / 255 + 1 > cap) return 0;
				uint8_t *tok = dst + op++;
				*tok = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
				if (lit >= 15) { size_t l = lit - 15; while (l >= 255) { dst[op++] = 255; l -= 255; } dst[op++] = (uint8_t)l; }
				memcpy(dst + op, src + anchor, lit); op += lit;
				size_t off = ip - r;
				dst[op++] = (uint8_t)(off & 0xFF); dst[op++] = (uint8_t)(off >> 8);
				*tok |= (uint8_t)(m >= 15 ? 15 : m);
				if (m >= 15) { size_t l = m - 15; while (l >= 255) { dst[op++] = 255; l -= 255; } dst[op++] = (uint8_t)l; }
				ip += ml; anchor = ip;
				continue;
			}
			ip++;
		}
	}
	size_t lit = n - anchor;
	if (op + 1 + lit + lit / 255 + 1 > cap) return 0;
	dst[op++] = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
	if (lit >= 15) { size_t l = lit - 15; while (l >= 255) { dst[op++] = 255; l -= 255; } dst[op++] = (uint8_t)l; }
	memcpy(dst + op, src + anchor, lit); op += lit;
	return op;
}

// Returns the decompressed size, or -1 on malformed input / output overflow.
static inline long lz4m_decompress(const uint8_t *src, size_t sn, uint8_t *dst, size_t dcap) {
	size_t ip = 0, op = 0;
	while (ip < sn) {
		uint8_t tok = src[ip++];
		size_t lit = tok >> 4;
		if (lit == 15) { uint8_t s; do { if (ip >= sn) return -1; s = src[ip++]; lit += s; } while (s == 255); }
		if (lit > sn - ip || lit > dcap - op) return -1;
		memcpy(dst + op, src + ip, lit); ip += lit; op += lit;
		if (ip >= sn) break;                       // last sequence: literals only
		if (sn - ip < 2) return -1;
		size_t off = (size_t)src[ip] | ((size_t)src[ip + 1] << 8); ip += 2;
		if (off == 0 || off > op) return -1;
		size_t ml = tok & 15;
		if (ml == 15) { uint8_t s; do { if (ip >= sn) return -1; s = src[ip++]; ml += s; } while (s == 255); }
		ml += 4;
		if (ml > dcap - op) return -1;
		const uint8_t *m = dst + op - off;
		if (off >= ml) memcpy(dst + op, m, ml);
		else for (size_t i = 0; i < ml; ++i) dst[op + i] = m[i];
		op += ml;
	}
	return (long)op;
}

#endif
