#include "../../engine/common/q_shared.h"
#include "../../engine/core/qcommon.h"
#include "../../game/ai/ai_main.h"

// Stubs for missing AI/botlib functions to allow linking

ai_manager_t ai_manager;

void G_InitGameInterface(void) {}
void G_ShutdownGameInterface(void) {}
void AI_Init(void) {}
void AI_Shutdown(void) {}
void BotAIStartFrame(int time) {}
nav_mesh_t *Nav_LoadMesh(const char *filename) { return NULL; }
void AI_UpdateEntity(int entNum, void *ent) {}
bot_controller_t* AI_GetBot(int clientNum) { return NULL; }
int BotChar_LoadCharacter(const char *name, float skill) { return 0; }
int BotChar_GetDefaultCharacter(void) { return 0; }
void BotChar_FreeCharacter(int handle) {}
float BotChar_GetFloat(int handle, const char *name) { return 0.0f; }
int BotChar_GetInt(int handle, const char *name) { return 0; }
void BotChar_GetString(int handle, const char *name, char *buffer, int size) { if (size > 0) buffer[0] = 0; }
