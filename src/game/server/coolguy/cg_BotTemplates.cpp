#include "cbase.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "tf_gamerules.h"

extern IFileSystem* filesystem;

KeyValues* g_KVBotTemplates = nullptr;

bool LoadBotTemplates()
{
    if (g_KVBotTemplates)
    {
        g_KVBotTemplates->deleteThis();
        g_KVBotTemplates = nullptr;
    }

    g_KVBotTemplates = new KeyValues("Templates");

    if (!g_KVBotTemplates)
        return false;

    FileFindHandle_t findHandle;
    const char* pFileName = filesystem->FindFirst(
        "scripts/population/*.pop",
        &findHandle
    );

    if (!pFileName)
    {
        filesystem->FindClose(findHandle);
        return true;
    }

    do
    {
        char filePath[MAX_PATH];
        V_snprintf(
            filePath,
            sizeof(filePath),
            "scripts/population/%s",
            pFileName
        );

        KeyValues* pFileKV = new KeyValues("WaveSchedule");

        if (!pFileKV->LoadFromFile(filesystem, filePath, "GAME"))
        {
            pFileKV->deleteThis();
            continue;
        }

        KeyValues* pTemplates = pFileKV->FindKey("Templates");

        if (pTemplates)
        {
            for (KeyValues* pTemplate = pTemplates->GetFirstSubKey();
                pTemplate;
                pTemplate = pTemplate->GetNextKey())
            {
                if (g_KVBotTemplates->FindKey(
                    pTemplate->GetName(), false))
                {
                    continue;
                }

                KeyValues* pTemplateCopy = pTemplate->MakeCopy();

                if (pTemplateCopy)
                    g_KVBotTemplates->AddSubKey(pTemplateCopy);
            }
        }

        pFileKV->deleteThis();

    } while ((pFileName = filesystem->FindNext(findHandle)) != nullptr);

    filesystem->FindClose(findHandle);

    return true;
}
