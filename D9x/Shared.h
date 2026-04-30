#pragma once
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

// ================= LOG FUNCTIONS =================
extern FILE* g_log;
void Log(const char* msg);
void LogF(const char* fmt, ...);

// ================= GLOBAL VARIABLES =================
extern int g_drawCallsPerSec;
extern int g_drawCallsPerFrame;
extern int g_textDrawCallsPerSec;
extern int g_textDrawCallsPerFrame;
extern int g_confirmedTextDrawsTotal;
extern int g_devicesCreated;
extern int g_devicesAlive;
extern int g_resets;
extern int g_presentCalls;
extern UINT g_maxPrim;

extern int g_DIP;
extern int g_DP;
extern int g_RT;
extern int g_TEX;
extern int g_VS;
extern int g_PS;
extern int g_STREAM;
extern int g_IDX;