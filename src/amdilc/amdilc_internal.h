#ifndef AMDILC_INTERNAL_H_
#define AMDILC_INTERNAL_H_

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "amdil/amdil.h"
#include "logger.h"
#include "amdilc.h"

typedef uint32_t Token;

typedef struct {
    uint32_t registerNum;
    uint8_t registerType;
    uint8_t component[4];
    bool clamp;
    uint8_t shiftScale;
} Destination;

typedef struct {
    uint32_t registerNum;
    uint8_t registerType;
    uint8_t swizzle[4];
    bool negate[4];
    bool invert;
    bool bias;
    bool x2;
    bool sign;
    bool abs;
    uint8_t divComp;
    bool clamp;
} Source;

typedef struct {
    uint16_t opcode;
    uint16_t control;
    uint32_t dstCount;
    Destination* dsts;
    uint32_t srcCount;
    Source* srcs;
    uint32_t extraCount;
    Token* extras;
} Instruction;

typedef struct {
    uint8_t clientType;
    uint8_t majorVersion;
    uint8_t minorVersion;
    uint8_t shaderType;
    bool multipass;
    bool realtime;
    uint32_t instrCount;
    Instruction* instrs;
} Kernel;

inline uint32_t getBits(
    uint32_t dword,
    uint32_t firstBit,
    uint32_t lastBit)
{
    assert(firstBit >= 0 && lastBit < 32 && firstBit <= lastBit);

    return (dword >> firstBit) & (0xFFFFFFFF >> (32 - (lastBit - firstBit + 1)));
}

inline uint32_t getBit(
    uint32_t dword,
    uint32_t bit)
{
    return getBits(dword, bit, bit);
}

Kernel* ilcDecodeStream(
    const Token* tokens,
    uint32_t count);

void ilcDumpKernel(
    const Kernel* kernel);

uint32_t* ilcCompileKernel(
    uint32_t* size,
    const Kernel* kernel);

#endif // AMDILC_INTERNAL_H_
