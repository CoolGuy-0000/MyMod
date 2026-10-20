#include "cbase.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "tf_gamerules.h"

extern KeyValues* g_KVBotTemplates;
bool LoadBotTemplates();

class CCGSystem : public CAutoGameSystem
{
public:
    CCGSystem()
        : CAutoGameSystem("CCGSystem")
    {
    }

    void LevelInitPostEntity() override
    {
        if (g_KVBotTemplates)
        {
            g_KVBotTemplates->deleteThis();
            g_KVBotTemplates = nullptr;
        }

        if (!TFGameRules() || !TFGameRules()->IsMannVsMachineMode())
            return;

        if (!LoadBotTemplates())
        {
            Warning("[BotTemplates] Failed to load bot templates.\n");
            return;
        }

        Msg("[BotTemplates] Bot templates loaded for MvM.\n");
    }
};

static CCGSystem g_CCGSystem;