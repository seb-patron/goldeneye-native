#ifndef GE_BOT_MEMORY_H
#define GE_BOT_MEMORY_H

/* Places this bot has been stuck before, remembered across runs. See ge_bot_memory.c. */

void        geBotMemoryLoad(const char *level);
void        geBotMemorySave(void);
void        geBotMemoryRecord(float x, float z, const char *fix);
int         geBotMemoryCount(void);
float       geBotMemoryPenalty(float x, float z, float to_x, float to_z);
const char *geBotMemoryFixNear(float x, float z);

#endif
