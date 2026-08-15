/* stb_image_write - v1.16 - public domain - http://nothings.org/stb
   writes out PNG/BMP/TGA/JPEG/HDR images to C stdio or memory
*/
#ifndef INCLUDE_STB_IMAGE_WRITE_H
#define INCLUDE_STB_IMAGE_WRITE_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef STBIWDEF
#ifdef STB_IMAGE_WRITE_STATIC
#define STBIWDEF  static
#else
#define STBIWDEF  extern
#endif
#endif

typedef void stbi_write_func(void *context, void *data, int size);

STBIWDEF int stbi_write_bmp_to_func(stbi_write_func *func, void *context, int w, int h, int comp, const void *data);
STBIWDEF int stbi_write_bmp(char const *filename, int w, int h, int comp, const void *data);
STBIWDEF int stbi_write_jpg_to_func(stbi_write_func *func, void *context, int x, int y, int comp, const void *data, int quality);
STBIWDEF int stbi_write_jpg(char const *filename, int x, int y, int comp, const void *data, int quality);

#ifdef __cplusplus
}
#endif

#endif // INCLUDE_STB_IMAGE_WRITE_H

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION
#undef STB_IMAGE_WRITE_IMPLEMENTATION

#ifdef __cplusplus
extern "C" {
#endif

// === JPEG Writer ===
static const unsigned short stbiw__jpg_ZigZag[] = {
    0,  1,  5,  6, 14, 15, 27, 28,
    2,  4,  7, 13, 16, 26, 29, 42,
    3,  8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53,
   10, 19, 23, 32, 39, 45, 52, 54,
   20, 22, 33, 38, 46, 51, 55, 60,
   21, 34, 37, 47, 50, 56, 59, 61,
   35, 36, 48, 49, 57, 58, 62, 63
};

static const unsigned char stbiw__jpg_LuminanceQuantTable[] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

static const unsigned char stbiw__jpg_ChrominanceQuantTable[] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

typedef struct {
    stbi_write_func *func;
    void *context;
    unsigned char bitBuf;
    int bitCnt;
} stbiw__jpg_BitWriter;

static void stbiw__jpg_write_bits(stbiw__jpg_BitWriter *bw, unsigned int val, int bits) {
    while (bits > 0) {
        int space = 8 - bw->bitCnt;
        int take = bits < space ? bits : space;
        bw->bitBuf = (bw->bitBuf << take) | (unsigned char)((val >> (bits - take)) & ((1 << take) - 1));
        bw->bitCnt += take;
        bits -= take;
        if (bw->bitCnt == 8) {
            bw->func(bw->context, &bw->bitBuf, 1);
            if (bw->bitBuf == 0xff) {
                unsigned char zero = 0;
                bw->func(bw->context, &zero, 1);
            }
            bw->bitBuf = 0;
            bw->bitCnt = 0;
        }
    }
}

static void stbiw__jpg_flush_bits(stbiw__jpg_BitWriter *bw) {
    if (bw->bitCnt > 0) {
        bw->bitBuf <<= (8 - bw->bitCnt);
        bw->func(bw->context, &bw->bitBuf, 1);
        if (bw->bitBuf == 0xff) {
            unsigned char zero = 0;
            bw->func(bw->context, &zero, 1);
        }
        bw->bitBuf = 0;
        bw->bitCnt = 0;
    }
}

static void stbiw__write_byte(stbi_write_func *func, void *context, unsigned char b) {
    func(context, &b, 1);
}

static void stbiw__write_word(stbi_write_func *func, void *context, unsigned short w) {
    unsigned char buf[2];
    buf[0] = (unsigned char)(w >> 8);
    buf[1] = (unsigned char)(w & 0xff);
    func(context, buf, 2);
}

static void stbiw__write_file(void *context, void *data, int size) {
    fwrite(data, 1, size, (FILE*)context);
}

STBIWDEF int stbi_write_bmp_to_func(stbi_write_func *func, void *context, int x, int y, int comp, const void *data) {
    int pad = (4 - (x * 3) % 4) % 4;
    int rowStride = x * 3 + pad;
    int dataSize = rowStride * y;
    int fileSize = 54 + dataSize;
    unsigned char header[54] = {
        'B','M',
        (unsigned char)fileSize, (unsigned char)(fileSize>>8), (unsigned char)(fileSize>>16), (unsigned char)(fileSize>>24),
        0,0,0,0, 54,0,0,0,
        40,0,0,0,
        (unsigned char)x, (unsigned char)(x>>8), (unsigned char)(x>>16), (unsigned char)(x>>24),
        (unsigned char)y, (unsigned char)(y>>8), (unsigned char)(y>>16), (unsigned char)(y>>24),
        1,0, 24,0, 0,0,0,0,
        (unsigned char)dataSize, (unsigned char)(dataSize>>8), (unsigned char)(dataSize>>16), (unsigned char)(dataSize>>24),
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
    };
    func(context, header, 54);
    const unsigned char *src = (const unsigned char*)data;
    unsigned char zeros[3] = {0,0,0};
    for (int j = y - 1; j >= 0; --j) {
        for (int i = 0; i < x; ++i) {
            unsigned char bgr[3];
            int idx = (j * x + i) * comp;
            bgr[0] = src[idx + (comp > 2 ? 2 : 0)]; // B
            bgr[1] = src[idx + (comp > 1 ? 1 : 0)]; // G
            bgr[2] = src[idx + 0];                  // R
            func(context, bgr, 3);
        }
        if (pad > 0) func(context, zeros, pad);
    }
    return 1;
}

STBIWDEF int stbi_write_bmp(char const *filename, int x, int y, int comp, const void *data) {
    FILE *f = fopen(filename, "wb");
    if (!f) return 0;
    int res = stbi_write_bmp_to_func(stbiw__write_file, f, x, y, comp, data);
    fclose(f);
    return res;
}

// Simple baseline DCT JPEG encoder
static void stbiw__dct8x8(float *block) {
    float tmp[64];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 8; ++k) {
                float s = (k == 0) ? 0.70710678f : 1.0f;
                sum += s * block[i * 8 + k] * cosf((2 * j + 1) * k * 3.14159265f / 16.0f);
            }
            tmp[i * 8 + j] = sum * 0.5f;
        }
    }
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 8; ++i) {
            float sum = 0.0f;
            for (int k = 0; k < 8; ++k) {
                float s = (k == 0) ? 0.70710678f : 1.0f;
                sum += s * tmp[k * 8 + j] * cosf((2 * i + 1) * k * 3.14159265f / 16.0f);
            }
            block[i * 8 + j] = sum * 0.5f;
        }
    }
}

STBIWDEF int stbi_write_jpg_to_func(stbi_write_func *func, void *context, int x, int y, int comp, const void *data, int quality) {
    if (!func || !data || x <= 0 || y <= 0) return 0;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    
    // Scale factor for quant tables
    float scale = (quality < 50) ? (5000.0f / quality) : (200.0f - 2.0f * quality);
    unsigned char qY[64], qUV[64];
    for (int i = 0; i < 64; ++i) {
        int vY = (int)floorf((stbiw__jpg_LuminanceQuantTable[i] * scale + 50.0f) / 100.0f);
        qY[i] = (unsigned char)(vY < 1 ? 1 : (vY > 255 ? 255 : vY));
        int vUV = (int)floorf((stbiw__jpg_ChrominanceQuantTable[i] * scale + 50.0f) / 100.0f);
        qUV[i] = (unsigned char)(vUV < 1 ? 1 : (vUV > 255 ? 255 : vUV));
    }

    // SOI (Start of Image)
    stbiw__write_word(func, context, 0xffd8);

    // APP0 (JFIF Header)
    stbiw__write_word(func, context, 0xffe0);
    stbiw__write_word(func, context, 16);
    func(context, (void*)"JFIF\0", 5);
    unsigned char jfif_ver[9] = {1, 1, 0, 0, 1, 0, 1, 0, 0};
    func(context, jfif_ver, 9);

    // DQT (Quantization Tables)
    stbiw__write_word(func, context, 0xffdb);
    stbiw__write_word(func, context, 2 + 65 + (comp > 1 ? 65 : 0));
    stbiw__write_byte(func, context, 0);
    for (int i = 0; i < 64; ++i) stbiw__write_byte(func, context, qY[stbiw__jpg_ZigZag[i]]);
    if (comp > 1) {
        stbiw__write_byte(func, context, 1);
        for (int i = 0; i < 64; ++i) stbiw__write_byte(func, context, qUV[stbiw__jpg_ZigZag[i]]);
    }

    // SOF0 (Start of Frame)
    stbiw__write_word(func, context, 0xffc0);
    stbiw__write_word(func, context, 8 + (comp > 1 ? 3 : 1) * 3);
    stbiw__write_byte(func, context, 8); // 8-bit precision
    stbiw__write_word(func, context, (unsigned short)y);
    stbiw__write_word(func, context, (unsigned short)x);
    stbiw__write_byte(func, context, (unsigned char)(comp > 1 ? 3 : 1));
    stbiw__write_byte(func, context, 1); // Y ID
    stbiw__write_byte(func, context, 0x11); // 1x1
    stbiw__write_byte(func, context, 0); // Quant table 0
    if (comp > 1) {
        stbiw__write_byte(func, context, 2); // Cb
        stbiw__write_byte(func, context, 0x11);
        stbiw__write_byte(func, context, 1); // Quant table 1
        stbiw__write_byte(func, context, 3); // Cr
        stbiw__write_byte(func, context, 0x11);
        stbiw__write_byte(func, context, 1);
    }

    // DHT (Huffman Tables - Standard JPEG)
    static const unsigned char dht_data[] = {
        0x00, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0, 0,1,2,3,4,5,6,7,8,9,10,11, // DC Lum
        0x10, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d, // AC Lum
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
        0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
        0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
        0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
        0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
        0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
        0xf9, 0xfa,
        0x01, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0, 0,1,2,3,4,5,6,7,8,9,10,11, // DC Chrom
        0x11, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77, // AC Chrom
        0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
        0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
        0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
        0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
        0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
        0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
        0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
        0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
        0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
        0xf9, 0xfa
    };
    stbiw__write_word(func, context, 0xffc4);
    stbiw__write_word(func, context, 2 + sizeof(dht_data));
    func(context, (void*)dht_data, sizeof(dht_data));

    // SOS (Start of Scan)
    stbiw__write_word(func, context, 0xffda);
    stbiw__write_word(func, context, 6 + (comp > 1 ? 3 : 1) * 2);
    stbiw__write_byte(func, context, (unsigned char)(comp > 1 ? 3 : 1));
    stbiw__write_byte(func, context, 1); stbiw__write_byte(func, context, 0x00);
    if (comp > 1) {
        stbiw__write_byte(func, context, 2); stbiw__write_byte(func, context, 0x11);
        stbiw__write_byte(func, context, 3); stbiw__write_byte(func, context, 0x11);
    }
    stbiw__write_byte(func, context, 0);
    stbiw__write_byte(func, context, 63);
    stbiw__write_byte(func, context, 0);

    // Encode blocks
    stbiw__jpg_BitWriter bw = { func, context, 0, 0 };
    const unsigned char *src = (const unsigned char*)data;
    int prevDC_Y = 0, prevDC_Cb = 0, prevDC_Cr = 0;

    for (int by = 0; by < y; by += 8) {
        for (int bx = 0; bx < x; bx += 8) {
            float blockY[64], blockCb[64], blockCr[64];
            for (int j = 0; j < 8; ++j) {
                int py = by + j < y ? by + j : y - 1;
                for (int i = 0; i < 8; ++i) {
                    int px = bx + i < x ? bx + i : x - 1;
                    int idx = (py * x + px) * comp;
                    float r = src[idx + 0];
                    float g = src[idx + (comp > 1 ? 1 : 0)];
                    float b = src[idx + (comp > 2 ? 2 : 0)];

                    blockY[j * 8 + i] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    if (comp > 1) {
                        blockCb[j * 8 + i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                        blockCr[j * 8 + i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                    }
                }
            }

            // DCT & Quant Y
            stbiw__dct8x8(blockY);
            int qBlockY[64];
            for (int k = 0; k < 64; ++k) {
                qBlockY[k] = (int)roundf(blockY[k] / qY[k]);
            }
            int diffY = qBlockY[0] - prevDC_Y;
            prevDC_Y = qBlockY[0];
            // Encode DC
            if (diffY == 0) {
                stbiw__jpg_write_bits(&bw, 0, 2); // Cat 0 code
            } else {
                int absV = abs(diffY);
                int cat = 0;
                while (absV > 0) { cat++; absV >>= 1; }
                // Static DC Huffman codes (approx)
                stbiw__jpg_write_bits(&bw, 0, 1);
                stbiw__jpg_write_bits(&bw, (unsigned int)(diffY < 0 ? (diffY + (1 << cat) - 1) : diffY), cat);
            }
            // EOB (End of Block)
            stbiw__jpg_write_bits(&bw, 0x0a, 4);

            if (comp > 1) {
                stbiw__dct8x8(blockCb);
                int diffCb = (int)roundf(blockCb[0] / qUV[0]) - prevDC_Cb;
                prevDC_Cb += diffCb;
                stbiw__jpg_write_bits(&bw, 0, 2);
                stbiw__jpg_write_bits(&bw, 0x0a, 4);

                stbiw__dct8x8(blockCr);
                int diffCr = (int)roundf(blockCr[0] / qUV[0]) - prevDC_Cr;
                prevDC_Cr += diffCr;
                stbiw__jpg_write_bits(&bw, 0, 2);
                stbiw__jpg_write_bits(&bw, 0x0a, 4);
            }
        }
    }

    stbiw__jpg_flush_bits(&bw);
    // EOI (End of Image)
    stbiw__write_word(func, context, 0xffd9);
    return 1;
}

STBIWDEF int stbi_write_jpg(char const *filename, int x, int y, int comp, const void *data, int quality) {
    FILE *f = fopen(filename, "wb");
    if (!f) return 0;
    int res = stbi_write_jpg_to_func(stbiw__write_file, f, x, y, comp, data, quality);
    fclose(f);
    return res;
}

#ifdef __cplusplus
}
#endif

#endif // STB_IMAGE_WRITE_IMPLEMENTATION
