#include "cbase.h"
#include "filesystem.h"

#include "KeyValues.h"

#include "player_vs_environment/tf_population_manager.h"
#include "player_vs_environment/tf_populator_spawners.h"

#define BRIDGE_MAX_ARGS       32
#define BRIDGE_STRING_LENGTH  256

enum BridgeArgType
{
    BRIDGE_ARG_NONE = 0,
    BRIDGE_ARG_INT,
    BRIDGE_ARG_FLOAT,
    BRIDGE_ARG_VECTOR,
    BRIDGE_ARG_STRING
};

struct BridgeArg
{
    BridgeArgType type;

    int iValue;
    float flValue;
    Vector vecValue;
    char szValue[BRIDGE_STRING_LENGTH];
};

extern KeyValues* g_KVBotTemplates;

class CServerBridge : public CBaseEntity
{
public:
    DECLARE_CLASS(CServerBridge, CBaseEntity);
    DECLARE_DATADESC();

    CServerBridge()
    {
        ClearArguments();
        ClearReturnValue();
    }

    void ClearArguments()
    {
        for (int i = 0; i < BRIDGE_MAX_ARGS; ++i)
        {
            m_Args[i].type = BRIDGE_ARG_NONE;
            m_Args[i].iValue = 0;
            m_Args[i].flValue = 0.0f;
            m_Args[i].vecValue.Init();
            m_Args[i].szValue[0] = '\0';
        }

        m_nArgCount = 0;
    }
    void ClearReturnValue()
    {
        m_iReturn = 0;
        m_flReturn = 0.0f;
        m_vecReturn.Init();
        m_szReturn[0] = '\0';
        m_iReturnType = BRIDGE_ARG_NONE;
    }

    bool PushInt(int value)
    {
        if (m_nArgCount >= BRIDGE_MAX_ARGS)
            return false;

        BridgeArg& arg = m_Args[m_nArgCount++];
        arg.type = BRIDGE_ARG_INT;
        arg.iValue = value;
        return true;
    }

    bool PushFloat(float value)
    {
        if (m_nArgCount >= BRIDGE_MAX_ARGS)
            return false;

        BridgeArg& arg = m_Args[m_nArgCount++];
        arg.type = BRIDGE_ARG_FLOAT;
        arg.flValue = value;
        return true;
    }

    bool PushVector(const Vector& value)
    {
        if (m_nArgCount >= BRIDGE_MAX_ARGS)
            return false;

        BridgeArg& arg = m_Args[m_nArgCount++];
        arg.type = BRIDGE_ARG_VECTOR;
        arg.vecValue = value;
        return true;
    }

    bool PushString(const char* value)
    {
        if (m_nArgCount >= BRIDGE_MAX_ARGS)
            return false;

        BridgeArg& arg = m_Args[m_nArgCount++];
        arg.type = BRIDGE_ARG_STRING;

        Q_strncpy(
            arg.szValue,
            value ? value : "",
            sizeof(arg.szValue)
        );

        return true;
    }

    void Spawn() override
    {
        BaseClass::Spawn();
    }

    bool AcceptInput(
        const char* inputName,
        CBaseEntity* activator,
        CBaseEntity* caller,
        variant_t value,
        int outputID) override
    {
        if (FStrEq(inputName, "PushArgInt"))
        {
            if (!value.Convert(FIELD_INTEGER))return false;
            return PushInt(value.Int());
        }
        else if (FStrEq(inputName, "PushArgFloat"))
        {
            if (!value.Convert(FIELD_FLOAT))return false;
            return PushFloat(value.Float());
        }
        else if (FStrEq(inputName, "PushArgVector"))
        {
            if (!value.Convert(FIELD_VECTOR))return false;
            Vector vec;
            value.Vector3D(vec);
            return PushVector(vec);
        }
        else if (FStrEq(inputName, "PushArgString"))
        {
            if (!value.Convert(FIELD_STRING))return false;
            return PushString(value.String());
        }



        else if (FStrEq(inputName, "EntSetHeath")) {
            m_nArgCount = 0;

            auto edict_t = engine->PEntityOfEntIndex(m_Args[0].iValue);
            CBaseEntity* ent;
            if (edict_t && (ent = edict_t->GetUnknown()->GetBaseEntity())) {
                ent->SetHealth(m_Args[1].iValue);
            }

            return true;
        }
        else if (FStrEq(inputName, "GetBotTemplateName")) {
            m_nArgCount = 0;

            if (!g_KVBotTemplates)
            {
                m_szReturn[0] = '\0';
                return true;
            }

            int index = m_Args[0].iValue;

            if (index < 0)
            {
                m_szReturn[0] = '\0';
                return true;
            }

            KeyValues* pTemplate = g_KVBotTemplates->GetFirstSubKey();

            for (int i = 0; pTemplate != nullptr && i < index; i++)
            {
                pTemplate = pTemplate->GetNextKey();
            }

            if (!pTemplate)
            {
                m_szReturn[0] = '\0';
                return true;
            }

            V_strncpy(m_szReturn, pTemplate->GetName(), sizeof(m_szReturn));

            return true;
        }
        else if (FStrEq(inputName, "GetBotTemplateIndex")) {
            m_nArgCount = 0;

            if (!g_KVBotTemplates || !m_Args[0].szValue)
            {
                m_iReturn = -1;
                return true;
            }

            const char* templateName = m_Args[0].szValue;

            int index = 0;

            for (KeyValues* pTemplate = g_KVBotTemplates->GetFirstSubKey();
                pTemplate != nullptr;
                pTemplate = pTemplate->GetNextKey(), index++)
            {
                if (FStrEq(pTemplate->GetName(), templateName))
                {
                    m_iReturn = index;
                    return true;
                }
            }

            // Template not found.
            m_iReturn = -1;
            return true;
        }
        else if (FStrEq(inputName, "SpawnBotWithTemplate")) {
            m_nArgCount = 0;

            KeyValues* pTemplate = g_KVBotTemplates->FindKey(m_Args[0].szValue);
            if (pTemplate && g_pPopulationManager) {
                CTFBotSpawner* botSpawner = new CTFBotSpawner(g_pPopulationManager->GetCurrentWave());
                if (botSpawner->Parse(pTemplate)) {
                    botSpawner->Spawn(m_Args[1].vecValue, NULL);
                }
                delete botSpawner;
            }
        }

        return BaseClass::AcceptInput(
            inputName, activator, caller, value, outputID);
    }
public:
    BridgeArg m_Args[BRIDGE_MAX_ARGS];
    int m_nArgCount;

    uint32 m_iReturnType;
    int m_iReturn;
    float m_flReturn;
    Vector m_vecReturn;
    char m_szReturn[512];
    bool m_bReturn;
};

LINK_ENTITY_TO_CLASS(cg_server_bridge, CServerBridge);

BEGIN_DATADESC(CServerBridge)
    DEFINE_FIELD(m_iReturn, FIELD_INTEGER),
    DEFINE_FIELD(m_flReturn, FIELD_FLOAT),
    DEFINE_FIELD(m_vecReturn, FIELD_VECTOR),
    DEFINE_ARRAY(m_szReturn, FIELD_CHARACTER, 512),
    DEFINE_FIELD(m_bReturn, FIELD_BOOLEAN),
END_DATADESC()