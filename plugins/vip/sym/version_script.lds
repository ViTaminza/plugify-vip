{
    global:
        Plugify_*;

        /* engine binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings through pisexcompat, filled by the
           plugify module from the hudmenu plugin's exported GetClientMenuType/SetClientMenuType) */
        __hudmenu_*;

        /* permissions binding slots (defined by permissions_bindings, filled by the plugify module
           from the permissions plugin's exported methods) - the cookie storage of
           VIP_Get/SetClientCookie */
        __permissions_*;

        /* Exported methods (vip.pplugin, group by group). Everything in plugins/vip/include is
           generated from the manifest with bin/plugify-gen - keep this list in sync. */
        VIP_IsVIPLoaded;
        VIP_GetTranslate;

        VIP_IsClientVIP;
        VIP_GetClientAccessTime;
        VIP_SetClientAccessTime;
        VIP_GetClientVIPGroup;
        VIP_SetClientVIPGroup;
        VIP_GiveClientVIP;
        VIP_RemoveClientVIP;

        VIP_GetClientFeatureInt;
        VIP_GetClientFeatureBool;
        VIP_GetClientFeatureFloat;
        VIP_GetClientFeatureString;
        VIP_RegisterFeature;

        VIP_IsValidVIPGroup;

        VIP_PistolRound;
        VIP_WarmupPeriod;
        VIP_GetTotalRounds;

        VIP_SetClientCookie;
        VIP_GetClientCookie;

        VIP_PrintToCenter;

        VIP_OpenMenu;

        VIP_OnVIPLoaded_Register;
        VIP_OnVIPLoaded_Unregister;
        VIP_OnClientLoaded_Register;
        VIP_OnClientLoaded_Unregister;
        VIP_OnClientDisconnect_Register;
        VIP_OnClientDisconnect_Unregister;
        VIP_OnPlayerSpawn_Register;
        VIP_OnPlayerSpawn_Unregister;
        VIP_OnVIPClientRemoved_Register;
        VIP_OnVIPClientRemoved_Unregister;
        VIP_OnVIPClientAdded_Register;
        VIP_OnVIPClientAdded_Unregister;

    local: *;
};
