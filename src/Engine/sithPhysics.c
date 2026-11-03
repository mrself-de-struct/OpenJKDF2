#include "sithPhysics.h"

#include "General/stdMath.h"
#include "Primitives/rdMath.h"
#include "Engine/sithCollision.h"
#include "World/sithSurface.h"
#include "World/sithThing.h"
#include "World/sithSector.h"
#include "World/jkPlayer.h"
#include "Main/Main.h"
#include "Dw/dwCog.h" // Added: dwCog_droidCaps (DroidWorks slope rule)
#include "jk.h"

extern int sithCollision_bDwDebugColl; // Added: DW_DEBUG_COLL physics trace switch

void sithPhysics_FindFloor(SithThing *pThing, int bNoThingStateUpdate)
{
    int v4; // ecx
    SithCollision *v5; // eax
    flex_d_t v8; // st7
    flex_d_t v9; // st7
    SithCollision *i; // esi
    SithThing *v11; // edi
    rdFace *v12; // eax
    int searchFlags; // [esp+10h] [ebp-20h]
    flex_t range; // [esp+14h] [ebp-1Ch]
    rdVector3 direction; // [esp+18h] [ebp-18h] BYREF
    rdVector3 a1; // [esp+24h] [ebp-Ch] BYREF
    flex_t thinga; // [esp+34h] [ebp+4h]

    // Added: noclip
    if ((g_debugmodeFlags & DEBUGFLAG_NOCLIP) && pThing == sithPlayer_g_pLocalPlayerThing)
    {
        pThing->physicsParams.flags &= ~SITH_PF_USEGRAVITY;
        pThing->physicsParams.flags |= SITH_PF_FLY;

        sithThing_DetachThing(pThing);
        return;
    }

    range = 0.0;
    searchFlags = 0;
    if (!pThing->sector)
        return;

    // Added: DroidWorks (DroidWorks.exe 0x4592fc): a droid in a water sector searches for a floor
    // like anywhere else (it walks the bottom); there is no water-surface search, and
    // other things find no floor under water.
    int bDwWalksBottom = Main_bDroidWorks && pThing->type == SITH_THING_PLAYER;

    // There are no floors underwater, unless you're the player
    if (!Main_bDroidWorks && (pThing->sector->flags & SITH_SECTOR_UNDERWATER) && pThing->type == SITH_THING_PLAYER)
    {
        sithCollision_SearchForCollisions(pThing->sector, pThing, &pThing->position, &rdroid_zVector3, 0.05, 0.0, RAYCAST_1);
        v5 = sithCollision_PopStack();
        if ( v5 )
        {
            while ( (v5->type & SITHCOLLISION_ADJOINCROSS) == 0 || (v5->surface->pAdjoin->sector->flags & SITH_SECTOR_UNDERWATER) != 0 )
            {
                v5 = sithCollision_PopStack();
                if ( !v5 )
                    goto LABEL_8;
            }
            pThing->field_48 = v5->distance;
            pThing->physicsParams.flags |= SITH_PF_ONWATERSURFACE;
            sithCollision_DecreaseStackLevel();
        }
        else
        {
LABEL_8:
            sithCollision_DecreaseStackLevel();
            pThing->physicsParams.flags &= ~SITH_PF_ONWATERSURFACE;
        }
    }
    else if (!(pThing->sector->flags & SITH_SECTOR_UNDERWATER) || bDwWalksBottom)
    {
        if ( (pThing->physicsParams.flags & SITH_PF_WALLSTICK) == 0 )
        {
            direction.x = -0.0;
            direction.y = direction.x;
            direction.z = -1.0;
            searchFlags = RAYCAST_10;
        }
        else
        {
            rdVector_Neg3(&direction, &pThing->orient.uvec);
        }

        if ( bNoThingStateUpdate || pThing->attach_flags )
        {
            v9 = pThing->physicsParams.height;
            if ( v9 == 0.0 )
            {
                if ( pThing->renderData.type == RD_THING_MODEL3 )
                    v9 = pThing->renderData.model3->insertOffset.z;
                thinga = pThing->moveSize - -0.005;
                if ( v9 <= thinga )
                    v9 = thinga;
            }
            if ( (pThing->physicsParams.flags & (SITH_PF_FLOORSTICK|SITH_PF_WALLSTICK)) != 0 )
                v8 = v9 + v9;
            else
                v8 = v9 * 1.1;
        }
        else
        {
            v8 = pThing->moveSize - -0.005;
        }

        if ( v8 > 0.0 )
        {
            sithCollision_SearchForCollisions(pThing->sector, 0, &pThing->position, &direction, v8, 0.0, searchFlags | RAYCAST_2000 | RAYCAST_800 | RAYCAST_2);
            while ( 1 )
            {
                for ( i = sithCollision_PopStack(); i; i = sithCollision_PopStack() )
                {
                    if ( (i->type & SITHCOLLISION_WORLD) != 0 )
                    {
                        //printf("Attach to new surface? %x\n", i->surface->field_0);
                        sithThing_AttachThingToSurface(pThing, i->surface, bNoThingStateUpdate);
                        sithCollision_DecreaseStackLevel();
                        return;
                    }
                    if ( (i->type & SITHCOLLISION_THING) != 0 )
                    {
                        v11 = i->pThingCollided;
                        if ( v11 != pThing )
                        {
                            v12 = i->face;
                            if ( !v12 || !i->pMeshCollided )
                            {
                                sithCollision_DecreaseStackLevel();
                                return;
                            }
                            
                            // Track thing that can move
                            if ( (searchFlags & RAYCAST_10) == 0
                              || (rdMatrix_TransformVector34(&a1, &v12->normal, &v11->orient), rdVector_Dot3(&a1, &rdroid_zVector3) >= 0.6) )
                            {
                                sithThing_AttachThingToThingFace(pThing, v11, i->face, i->pMeshCollided->aVertices, bNoThingStateUpdate);
                                sithCollision_DecreaseStackLevel();
                                return;
                            }
                        }
                    }
                }
                sithCollision_DecreaseStackLevel();
                if ( range != 0.0 )
                    break;

                if ( pThing->type != SITH_THING_ACTOR && pThing->type != SITH_THING_PLAYER )
                    break;
                if ( pThing->moveSize == 0.0 )
                    break;
                range = pThing->moveSize;
                sithCollision_SearchForCollisions(pThing->sector, 0, &pThing->position, &direction, v8, range, searchFlags | RAYCAST_2000 | RAYCAST_800 | RAYCAST_2);
            }
        }
        if ( pThing->attach_flags )
            sithThing_DetachThing(pThing);
    }
}

// Inlined func

void sithPhysics_UpdateThing(SithThing *pThing, flex_t secDeltaTime)
{
    if (!pThing->sector)
        return;

    rdVector_Zero3(&pThing->physicsParams.deltaVelocity);
    rdVector_Zero3(&pThing->physicsParams.gravityForce);

    if ((pThing->type == SITH_THING_ACTOR || pThing->type == SITH_THING_PLAYER) 
        && (pThing->actorParams.flags & SITH_AF_COMBO_FREEZE))
    {
        rdVector_Zero3(&pThing->physicsParams.acceleration);
    }

    if (pThing->attach_flags & (SITH_ATTACH_THINGFACE | SITH_ATTACH_SURFACE))
    {
        sithPhysics_UpdateAttachedThingPhysics(pThing, secDeltaTime);
    }
    // Added: DroidWorks (DroidWorks.exe 0x45959f): an unattached player droid takes the player
    // path before the sector's underwater flag is looked at; droids walk the bottom, and
    // the player path applies the water drag and half gravity itself. The original's
    // player path is the 50 Hz rollover integrator of the same equations the general
    // path runs at the fixed physics step, so the general path is used here.
    else if ( Main_bDroidWorks && pThing->type == SITH_THING_PLAYER )
    {
        sithPhysics_UpdateThingPhysics(pThing, secDeltaTime);
    }
    else if (pThing->sector->flags & SITH_SECTOR_UNDERWATER)
    {
        sithPhysics_UpdateUnderwaterThingPhysics(pThing, secDeltaTime);
    }
#ifdef QOL_IMPROVEMENTS
    else if ( pThing->type == SITH_THING_PLAYER && (jkPlayer_bUseOldPlayerPhysics || sithNet_isMulti))
    {
#ifdef FIXED_TIMESTEP_PHYS
        if ((NEEDS_STEPPED_PHYS) && !jkPlayer_bUseOldPlayerPhysics) {
            // time stepping is handled elsewhere
            sithPhysics_UpdateThingPhysics(pThing, secDeltaTime);
        }
        else
        {
            sithPhysics_UpdatePlayerPhysics(pThing, secDeltaTime);
        }
#else
        sithPhysics_UpdatePlayerPhysics(pThing, secDeltaTime);
#endif
    }
#else
    else if ( pThing->type == SITH_THING_PLAYER )
    {
        sithPhysics_UpdatePlayerPhysics(pThing, secDeltaTime);
    }
#endif
    else
    {
        sithPhysics_UpdateThingPhysics(pThing, secDeltaTime);
    }
}

void sithPhysics_ApplyForce(SithThing *pThing, rdVector3 *force)
{
    // Added: noclip
    if (pThing == sithPlayer_g_pLocalPlayerThing && (g_debugmodeFlags & DEBUGFLAG_NOCLIP)) {
        return;
    }

    if ( pThing->moveType == SITH_MT_PHYSICS && pThing->physicsParams.mass > 0.0 )
    {
        flex_t invMass = 1.0 / pThing->physicsParams.mass;

        if ( force->z * invMass > 0.5 ) // TODO verify
            sithThing_DetachThing(pThing);

        rdVector_ScaleAdd3Acc(&pThing->physicsParams.vel, force, invMass);
        pThing->physicsParams.flags |= SITH_PF_8000;
    }
}

void sithPhysics_SetThingLook(SithThing *pThing, const rdVector3 *pNormal, flex_t secDeltaTime)
{
    flex_d_t v4; // st7
    flex_d_t v20; // st7

    v4 = stdMath_ClipNearZero(1.0 - rdVector_Dot3(&pThing->orient.uvec, pNormal));
    if ( v4 == 0.0 )
    {
        pThing->physicsParams.flags |= SITH_PF_100;
    }
    else if ( secDeltaTime == 0.0 )
    {
        // TODO: rdMatrix? Or are they just manually doing basis vectors?
        pThing->orient.uvec.x = pNormal->x;
        pThing->orient.uvec.y = pNormal->y;
        pThing->orient.uvec.z = pNormal->z;
        pThing->orient.rvec.x = (pThing->orient.lvec.y * pThing->orient.uvec.z) - (pThing->orient.lvec.z * pThing->orient.uvec.y);
        pThing->orient.rvec.y = (pThing->orient.lvec.z * pThing->orient.uvec.x) - (pThing->orient.lvec.x * pThing->orient.uvec.z);
        pThing->orient.rvec.z = (pThing->orient.lvec.x * pThing->orient.uvec.y) - (pThing->orient.lvec.y * pThing->orient.uvec.x);
        rdVector_Normalize3Acc(&pThing->orient.rvec);
        pThing->orient.lvec.x = (pThing->orient.rvec.z * pThing->orient.uvec.y) - (pThing->orient.rvec.y * pThing->orient.uvec.z);
        pThing->orient.lvec.y = (pThing->orient.rvec.x * pThing->orient.uvec.z) - (pThing->orient.rvec.z * pThing->orient.uvec.x);
        pThing->orient.lvec.z = (pThing->orient.rvec.y * pThing->orient.uvec.x) - (pThing->orient.rvec.x * pThing->orient.uvec.y);
        

        pThing->physicsParams.flags |= SITH_PF_100;
    }
    else
    {
        // TODO: rdMatrix? Or are they just manually doing basis vectors?
        v20 = secDeltaTime * 10.0;
        // Altered: the decompilation had the y and z components of pNormal swapped here.
        // Both JK.EXE (0x4f5e83) and DroidWorks.exe (0x459833) add pNormal->y to uvec.y
        // and pNormal->z to uvec.z.
        pThing->orient.uvec.x = pNormal->x * v20 + pThing->orient.uvec.x;
        pThing->orient.uvec.y = pNormal->y * v20 + pThing->orient.uvec.y;
        pThing->orient.uvec.z = pNormal->z * v20 + pThing->orient.uvec.z;
        rdVector_Normalize3Acc(&pThing->orient.uvec);
        pThing->orient.lvec.x = (pThing->orient.rvec.z * pThing->orient.uvec.y) - (pThing->orient.rvec.y * pThing->orient.uvec.z);
        pThing->orient.lvec.y = (pThing->orient.rvec.x * pThing->orient.uvec.z) - pThing->orient.rvec.z * pThing->orient.uvec.x;
        pThing->orient.lvec.z = (pThing->orient.rvec.y * pThing->orient.uvec.x) - (pThing->orient.rvec.x * pThing->orient.uvec.y);
        rdVector_Normalize3Acc(&pThing->orient.lvec);
        pThing->orient.rvec.x = (pThing->orient.lvec.y * pThing->orient.uvec.z) - (pThing->orient.lvec.z * pThing->orient.uvec.y);
        pThing->orient.rvec.y = (pThing->orient.lvec.z * pThing->orient.uvec.x) - (pThing->orient.lvec.x * pThing->orient.uvec.z);
        pThing->orient.rvec.z = (pThing->orient.lvec.x * pThing->orient.uvec.y) - (pThing->orient.lvec.y * pThing->orient.uvec.x);
    }
}

void sithPhysics_ApplyDrag(rdVector3 *pVelocity, flex_t drag, flex_t mag, flex_t secDeltaTime)
{
    if (mag == 0.0 || rdVector_Len3(pVelocity) >= mag)
    {
        if (drag != 0.0)
        {
            flex_d_t scaled = secDeltaTime * drag;
            if (scaled > 1.0)
                scaled = 1.0;

            rdVector_ScaleAdd3Acc(pVelocity, pVelocity, -scaled);
            
            rdMath_ClipVector3Acc(pVelocity, 0.00001);
        }
    }
    else
    {
        rdVector_Zero3(pVelocity);
    }
}

int sithPhysics_ParseArg(StdConffileArg *pArg, SithThing *pThing, int adjNum)
{
    flex32_t tmp;
    int tmpInt;
    flex32_t velx, vely, velz;

    switch ( adjNum )
    {
        case THINGPARAM_SURFDRAG:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 )
                return 0;
            pThing->physicsParams.surfDrag = tmp;
            return 1;
        case THINGPARAM_AIRDRAG:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 )
                return 0;
            pThing->physicsParams.airDrag = tmp;
            return 1;
        case THINGPARAM_STATICDRAG:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 )
                return 0;
            pThing->physicsParams.staticDrag = tmp;
            return 1;
        case THINGPARAM_MASS:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 )
                return 0;
            pThing->physicsParams.mass = tmp;
            return 1;
        case THINGPARAM_HEIGHT:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 )
                return 0;
            pThing->physicsParams.height = tmp;
            return 1;
        case THINGPARAM_PHYSFLAGS:
            if ( _sscanf(pArg->value, "%x", &tmpInt) != 1 )
                return 0;
            pThing->physicsParams.flags = tmpInt;
            return 1;
        case SITHTHING_ARG_MAXROTVEL:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 || pThing->moveType != SITH_MT_PHYSICS )
                return 0;
            pThing->physicsParams.maxRotationVelocity = tmp;
            return 1;
        case THINGPARAM_MAXVEL:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 || pThing->moveType != SITH_MT_PHYSICS )
                return 0;
            pThing->physicsParams.maxVelocity = tmp;
            return 1;
        case THINGPARAM_VEL:
            if (_sscanf(
                      pArg->value,
                      "(%f/%f/%f)",
                      &velx, &vely, &velz) != 3)
                return 0;
            pThing->physicsParams.vel.x = velx;
            pThing->physicsParams.vel.y = vely;
            pThing->physicsParams.vel.z = velz;
            return 1;
        case THINGPARAM_ANGVEL:
            if (_sscanf(
                      pArg->value,
                      "(%f/%f/%f)",
                      &velx, &vely, &velz) != 3)
                return 0;
            pThing->physicsParams.angularVelocity.x = velx;
            pThing->physicsParams.angularVelocity.y = vely;
            pThing->physicsParams.angularVelocity.z = velz;
            return 1;
        case THINGPARAM_ORIENTSPEED:
            tmp = _atof(pArg->value);
            if ( tmp < 0.0 || pThing->moveType != SITH_MT_PHYSICS )
                return 0;
            pThing->physicsParams.orientSpeed = tmp;
            return 1;
        case THINGPARAM_BUOYANCY:
            tmp = _atof(pArg->value);
            pThing->physicsParams.buoyancy = tmp;
            return 1;
        default:
            return 0;
    }
}

void sithPhysics_ResetThingMovement(SithThing *pThing)
{
    rdVector_Zero3(&pThing->physicsParams.vel);
    rdVector_Zero3(&pThing->physicsParams.angularVelocity);
    rdVector_Zero3(&pThing->physicsParams.field_1F8);
    rdVector_Zero3(&pThing->physicsParams.acceleration);
    rdVector_Zero3(&pThing->physicsParams.deltaVelocity);
    rdVector_Zero3(&pThing->field_268);
}

flex_t sithPhysics_GetThingHeight(SithThing *pThing)
{
    flex_d_t result; // st7
    flex_t v2; // [esp+4h] [ebp+4h]

    result = pThing->physicsParams.height;
    if ( result == 0.0 )
    {
        if ( pThing->renderData.type == RD_THING_MODEL3 )
            result = pThing->renderData.model3->insertOffset.z;
        v2 = pThing->moveSize - -0.005;
        if ( result <= v2 )
            result = v2;
    }
    return result;
}

// Added: render tick on which the DroidWorks local player's in-air thrust was last scaled
static uint32_t sithPhysics_dwAccelScaleTick = 0xFFFFFFFF;

// MOTS altered
void sithPhysics_UpdateThingPhysics(SithThing *pThing, flex_t secDeltaTime)
{
    rdVector3 a1a;
    rdVector3 a3;
    rdMatrix34 a;
    int bOverrideIdk = 0;
    flex_t zOverride = 0.0;

    rdVector_Zero3(&pThing->physicsParams.gravityForce);
    rdVector_Zero3(&a1a);

    if (pThing->physicsParams.flags & SITH_PF_USEANGULARTHRUST)
    {
        if (!rdVector_IsZero3(&pThing->physicsParams.angularVelocity))
        {
            sithPhysics_ApplyDrag(&pThing->physicsParams.angularVelocity, pThing->physicsParams.airDrag - -0.2, 0.0, secDeltaTime);
        }

        rdVector_ScaleAdd3Acc(&pThing->physicsParams.angularVelocity, &pThing->physicsParams.field_1F8, secDeltaTime);
        
        rdMath_ClampVector3Acc(&pThing->physicsParams.angularVelocity, -pThing->physicsParams.maxRotationVelocity, pThing->physicsParams.maxRotationVelocity);
        rdMath_ClipVector3Acc(&pThing->physicsParams.angularVelocity, 0.00001);
    }

    if (rdVector_IsZero3(&pThing->physicsParams.angularVelocity))
    {
        rdVector_Zero3(&a3);
    }
    else
    {
        rdVector_Scale3(&a3, &pThing->physicsParams.angularVelocity, secDeltaTime);
    }

    // MOTS added: weapon tracking?
#ifdef JKM_PARAMS
    if (pThing->type == SITH_THING_WEAPON && pThing->weaponParams.pTargetThing && pThing->weaponParams.field_38 != 0.0) {
        rdVector3 tmp;
        rdMatrix34 local_60;
        rdVector3 local_6c, local_78;

        rdVector_Sub3(&tmp, &pThing->weaponParams.pTargetThing->position, &pThing->position);
        flex_t fVar3 = secDeltaTime * pThing->weaponParams.field_38;

        if (-0.03 <= tmp.z) {
            if (tmp.z > 0.03) {
                zOverride = 1.0;
            }
        }
        else {
            zOverride = -1.0;
        }
        rdVector_Normalize3Acc(&tmp);
        rdMatrix_BuildFromLook34(&local_60,&tmp);
        rdMatrix_ExtractAngles34(&local_60,&local_6c);
        rdMatrix_Copy34(&local_60, &pThing->orient);
        rdMatrix_ExtractAngles34(&local_60,&local_78);
        tmp.y = local_6c.y - local_78.y;
        tmp.x = -local_78.x;
        tmp.z = -local_78.z;
        if (tmp.y > 180.0) {
            tmp.y = tmp.y - 360.0;
        }
        else if (tmp.y < -180.0) {
            tmp.y = tmp.y - -360.0;
        }
        flex_t fVar6 = tmp.y;
        if (tmp.y < 0.0) {
            fVar6 = -tmp.y;
        }
        if (fVar6 > fVar3) {
            fVar6 = tmp.y;
            if (tmp.y < 0.0) {
                fVar6 = -tmp.y;
            }
            tmp.y = (fVar3 / fVar6) * tmp.y;
        }
        bOverrideIdk = 1;
        rdVector_Copy3(&a3, &tmp);
    }
#endif

    if (!rdVector_IsZero3(&a3))
    {
        rdMatrix_BuildRotate34(&a, &a3);
        sithCollision_RotateThing(pThing, &a);

        if (pThing->physicsParams.flags & SITH_PF_FLY)
            rdMatrix_TransformVector34Acc(&pThing->physicsParams.vel, &a);

        if ( ((jkPlayer_currentTickIdx + (pThing->idx & 0xFF)) & 7) == 0 )
            rdMatrix_Normalize34(&pThing->orient);
    }

    // Added: DroidWorks (DroidWorks.exe 0x45a82b): a falling droid in a water sector drags 1.2x.
    int bDwPlayerInWater = Main_bDroidWorks
                        && pThing->type == SITH_THING_PLAYER
                        && (pThing->sector->flags & SITH_SECTOR_UNDERWATER) != 0;
    if ( pThing->physicsParams.airDrag != 0.0 )
    {
        flex_t airDrag = pThing->physicsParams.airDrag;
        if ( bDwPlayerInWater )
            airDrag *= 1.2;
        sithPhysics_ApplyDrag(&pThing->physicsParams.vel, airDrag, 0.0, secDeltaTime);
    }

    if (pThing->physicsParams.flags & SITH_PF_USETHRUST)
    {
        if (!(pThing->physicsParams.flags & SITH_PF_FLY))
        {
            // Added: DroidWorks — the original ran this once per frame on a thrust the controls
            // reset every frame, so in-air thrust was a flat 0.3 of the input at any
            // framerate. The fixed-step loop would compound the 0.3 once per step
            // (0.3^steps per frame), so for the local player scale once per rendered frame.
            if ( Main_bDroidWorks && pThing == sithPlayer_g_pLocalPlayerThing )
            {
                if ( sithPhysics_dwAccelScaleTick != jkPlayer_currentTickIdx )
                {
                    sithPhysics_dwAccelScaleTick = jkPlayer_currentTickIdx;
                    rdVector_Scale3Acc(&pThing->physicsParams.acceleration, 0.3);
                }
            }
            else
            {
                rdVector_Scale3Acc(&pThing->physicsParams.acceleration, 0.3);
            }
        }
        rdVector_Scale3(&a1a, &pThing->physicsParams.acceleration, secDeltaTime);
        rdMatrix_TransformVector34Acc(&a1a, &pThing->orient);
    }

    if (pThing->physicsParams.mass != 0.0 
        && (pThing->sector->flags & SITH_SECTOR_USETHRUST) 
        && !(pThing->physicsParams.flags & SITH_PF_NOTHRUST))
    {
        rdVector_ScaleAdd3Acc(&a1a, &pThing->sector->thrust, secDeltaTime);
    }

    if (pThing->physicsParams.mass != 0.0 
        && pThing->physicsParams.flags & SITH_PF_USEGRAVITY
        && !(pThing->sector->flags & SITH_SECTOR_NOGRAVITY))
    {
        flex_t gravity = sithWorld_g_pCurrentWorld->gravity * secDeltaTime;
        // Altered: DroidWorks (DroidWorks.exe 0x45a935): half gravity on a droid falling through water.
        if ( (pThing->physicsParams.flags & SITH_PF_PARTIALGRAVITY) != 0 || bDwPlayerInWater )
            gravity *= 0.5;
        a1a.z = a1a.z - gravity;
        pThing->physicsParams.gravityForce.z = -gravity;
    }

    rdVector_Add3Acc(&pThing->physicsParams.vel, &a1a);
#ifdef JKM_PARAMS
    if (bOverrideIdk) {
        pThing->physicsParams.vel.z = zOverride;
    }
#endif
    rdMath_ClipVector3Acc(&pThing->physicsParams.vel, 0.00001);

    if (!rdVector_IsZero3(&pThing->physicsParams.vel))
    {
        rdVector_Scale3(&pThing->physicsParams.deltaVelocity, &pThing->physicsParams.vel, secDeltaTime);
    }
}

// MOTS altered
void sithPhysics_UpdatePlayerPhysics(SithThing *pThing, flex_t msDeltaTime)
{
    rdMatrix34 a;
    rdVector3 a3;
    rdVector3 a1a;
    //int bOverrideIdk = 0; // Remove compiler warns
    flex_t zOverride = 0.0;

    rdVector_Zero3(&pThing->physicsParams.gravityForce);
    if (pThing->physicsParams.flags & SITH_PF_USEANGULARTHRUST)
    {
        if (!rdVector_IsZero3(&pThing->physicsParams.angularVelocity))
        {
            sithPhysics_ApplyDrag(&pThing->physicsParams.angularVelocity, pThing->physicsParams.airDrag - -0.2, 0.0, msDeltaTime);
        }

        rdVector_ScaleAdd3Acc(&pThing->physicsParams.angularVelocity, &pThing->physicsParams.field_1F8, msDeltaTime);

        rdMath_ClampVector3Acc(&pThing->physicsParams.angularVelocity, -pThing->physicsParams.maxRotationVelocity, pThing->physicsParams.maxRotationVelocity);
        rdMath_ClipVector3Acc(&pThing->physicsParams.angularVelocity, 0.00001);
    }

    if (rdVector_IsZero3(&pThing->physicsParams.angularVelocity))
    {
        rdVector_Zero3(&a3);
    }
    else
    {
        rdVector_Scale3(&a3, &pThing->physicsParams.angularVelocity, msDeltaTime);
    }

// MOTS added: weapon tracking? why is this here lol
#ifdef JKM_PARAMS
    if (pThing->type == SITH_THING_WEAPON && pThing->weaponParams.pTargetThing && pThing->weaponParams.field_38 != 0.0) {
        rdVector3 tmp;
        rdMatrix34 local_60;
        rdVector3 local_6c, local_78;

        rdVector_Sub3(&tmp, &pThing->weaponParams.pTargetThing->position, &pThing->position);
        flex_t fVar3 = msDeltaTime * pThing->weaponParams.field_38;

        rdVector_Normalize3Acc(&tmp);
        rdMatrix_BuildFromLook34(&local_60,&tmp);
        rdMatrix_ExtractAngles34(&local_60,&local_6c);
        rdMatrix_Copy34(&local_60, &pThing->orient);
        rdMatrix_ExtractAngles34(&local_60,&local_78);
        tmp.y = local_6c.y - local_78.y;
        tmp.x = -local_78.x;
        tmp.z = -local_78.z;
        if (tmp.y > 180.0) {
            tmp.y = tmp.y - 360.0;
        }
        else if (tmp.y < -180.0) {
            tmp.y = tmp.y - -360.0;
        }
        flex_t fVar6 = tmp.y;
        if (tmp.y < 0.0) {
            fVar6 = -tmp.y;
        }
        if (fVar6 > fVar3) {
            fVar6 = tmp.y;
            if (tmp.y < 0.0) {
                fVar6 = -tmp.y;
            }
            tmp.y = (fVar3 / fVar6) * tmp.y;
        }
        //bOverrideIdk = 1; // Remove compiler warns
        rdVector_Copy3(&a3, &tmp);
    }
#endif

    if (!rdVector_IsZero3(&a3))
    {
        rdMatrix_BuildRotate34(&a, &a3);
        sithCollision_RotateThing(pThing, &a);

        if (pThing->physicsParams.flags & SITH_PF_FLY)
            rdMatrix_TransformVector34Acc(&pThing->physicsParams.vel, &a);

        if ( ((jkPlayer_currentTickIdx + (pThing->idx & 0xFF)) & 7) == 0 )
            rdMatrix_Normalize34(&pThing->orient);
    }

    if (!(pThing->physicsParams.flags & SITH_PF_FLY))
    {
        rdVector_Scale3Acc(&pThing->physicsParams.acceleration, 0.3);
    }

    // I think all of this is specifically for multiplayer, so that player aThings
    // sync better between clients.
    flex_t rolloverCombine = msDeltaTime + pThing->physicsParams.physicsRolloverFrames;

    int32_t framesToApply = (int32_t)stdMath_Floor(rolloverCombine * OLDSTEP_TARGET_FPS + 0.5); // number of 50FPS steps passed
    pThing->physicsParams.physicsRolloverFrames = rolloverCombine - (flex_d_t)(unsigned int)framesToApply * OLDSTEP_DELTA_50FPS;

    for (int32_t i = framesToApply; i > 0; i--)
    {
        rdVector_Zero3(&a1a);
        if ( pThing->physicsParams.airDrag != 0.0 )
        {
            sithPhysics_ApplyDrag(&pThing->physicsParams.vel, pThing->physicsParams.airDrag, 0.0, OLDSTEP_DELTA_50FPS);
        }

        if (pThing->physicsParams.flags & SITH_PF_USETHRUST)
        {
            rdVector_Scale3(&a1a, &pThing->physicsParams.acceleration, OLDSTEP_DELTA_50FPS);
            rdMatrix_TransformVector34Acc(&a1a, &pThing->orient);
        }

        if ( pThing->physicsParams.mass != 0.0 )
        {
            if ((pThing->sector->flags & SITH_SECTOR_USETHRUST)
                && !(pThing->physicsParams.flags & SITH_PF_NOTHRUST))
            {
                rdVector_ScaleAdd3Acc(&a1a, &pThing->sector->thrust, OLDSTEP_DELTA_50FPS);
            }
        }

        if ( pThing->physicsParams.mass != 0.0 
             && (pThing->physicsParams.flags & SITH_PF_USEGRAVITY) 
             && !(pThing->sector->flags & SITH_SECTOR_NOGRAVITY) )
        {
            flex_t gravity = sithWorld_g_pCurrentWorld->gravity * OLDSTEP_DELTA_50FPS;
            if ( (pThing->physicsParams.flags & SITH_PF_PARTIALGRAVITY) != 0 )
                gravity = gravity * 0.5;
            a1a.z = a1a.z - gravity;
            pThing->physicsParams.gravityForce.z = -gravity;
        }
        rdVector_Add3Acc(&pThing->physicsParams.vel, &a1a);
        rdVector_ScaleAdd3Acc(&pThing->physicsParams.deltaVelocity, &pThing->physicsParams.vel, OLDSTEP_DELTA_50FPS);
    }
}

// MOTS altered
void sithPhysics_UpdateUnderwaterThingPhysics(SithThing *pThing, flex_t secDeltaTime)
{
    flex_d_t v35; // st6
    flex_d_t v51; // st7
    rdVector3 a1a; // [esp+24h] [ebp-48h] BYREF
    rdVector3 a3; // [esp+30h] [ebp-3Ch] BYREF
    rdMatrix34 tmpMat; // [esp+3Ch] [ebp-30h] BYREF

    rdVector_Zero3(&a1a);
    rdVector_Zero3(&pThing->physicsParams.gravityForce);
    if ( (pThing->physicsParams.flags & SITH_PF_USEANGULARTHRUST) != 0 )
    {
        if ( !rdVector_IsZero3(&pThing->physicsParams.angularVelocity) )
        {
            sithPhysics_ApplyDrag(&pThing->physicsParams.angularVelocity, pThing->physicsParams.airDrag - -0.2, 0.0, secDeltaTime);
        }
        rdVector_ScaleAdd3Acc(&pThing->physicsParams.angularVelocity, &pThing->physicsParams.field_1F8, secDeltaTime);
        rdVector_ClampValue3(&pThing->physicsParams.angularVelocity, pThing->physicsParams.maxRotationVelocity);
        rdVector_ClipPrecision3(&pThing->physicsParams.angularVelocity);
    }
    if ( rdVector_IsZero3(&pThing->physicsParams.angularVelocity) )
    {
        rdVector_Zero3(&a3);
    }
    else
    {
        rdVector_Scale3(&a3, &pThing->physicsParams.angularVelocity, secDeltaTime);
    }

    if (!rdVector_IsZero3(&a3))
    {
        rdMatrix_BuildRotate34(&tmpMat, &a3);
        sithCollision_RotateThing(pThing, &tmpMat);
        if ( (((jkPlayer_currentTickIdx & 0xFF) + (pThing->idx & 0xFF)) & 7) == 0 )
            rdMatrix_Normalize34(&pThing->orient);
    }
    if ( pThing->physicsParams.airDrag != 0.0 )
    {
        sithPhysics_ApplyDrag(&pThing->physicsParams.vel, pThing->physicsParams.airDrag * 4.0, 0.0, secDeltaTime);
    }
    if ( (pThing->physicsParams.flags & SITH_PF_USETHRUST) != 0 )
    {
        rdVector_Scale3Acc(&pThing->physicsParams.acceleration, 0.6);
        rdVector_Scale3(&a1a, &pThing->physicsParams.acceleration, secDeltaTime);
        rdMatrix_TransformVector34Acc(&a1a, &pThing->orient);
    }
    if ( pThing->physicsParams.mass != 0.0 && pThing->sector && (pThing->sector->flags & SITH_SECTOR_USETHRUST) && !(pThing->physicsParams.flags & SITH_PF_NOTHRUST) )
    {
        rdVector_ScaleAdd3Acc(&a1a, &pThing->sector->thrust, secDeltaTime);
    }

    if ( ((pThing->physicsParams.flags & SITH_PF_ONWATERSURFACE) == 0 || (pThing->flags & SITH_TF_DEAD) != 0) && (pThing->physicsParams.flags & SITH_PF_USEGRAVITY) != 0 )
    {
        v35 = sithWorld_g_pCurrentWorld->gravity * secDeltaTime * pThing->physicsParams.buoyancy;
        a1a.z -= v35;
        pThing->physicsParams.gravityForce.z -= v35;
    }
    rdVector_Add3Acc(&pThing->physicsParams.vel, &a1a);

    rdVector_ClipPrecision3(&pThing->physicsParams.vel);
    if ( !rdVector_IsZero3(&pThing->physicsParams.vel) )
    {
        rdVector_Scale3(&pThing->physicsParams.deltaVelocity, &pThing->physicsParams.vel, secDeltaTime);
    }
    if ( (pThing->physicsParams.flags & SITH_PF_ONWATERSURFACE) != 0 && pThing->physicsParams.acceleration.z >= 0.0 )
    {
        v51 = pThing->field_48 - 0.01;
        if ( pThing->physicsParams.deltaVelocity.z > 0.0 && pThing->physicsParams.deltaVelocity.z < (flex_d_t)secDeltaTime * 0.2 ) // verify first
            pThing->physicsParams.deltaVelocity.z = 0.0;
        if ( v51 > 0.0 )
        {
            if ( v51 >= secDeltaTime * 0.2 )
                v51 = secDeltaTime * 0.2;
            rdVector_ScaleAdd3Acc(&pThing->physicsParams.deltaVelocity, &rdroid_zVector3, v51);
        }
    }
}

// MOTS altered
void sithPhysics_UpdateAttachedThingPhysics(SithThing *pThing, flex_t secDeltaTime)
{   
    flex_t a2a; // [esp+0h] [ebp-94h]
    flex_t v144; // [esp+4h] [ebp-90h]
    flex_t possibly_undef_2; // [esp+1Ch] [ebp-78h]
    flex_t new_z; // [esp+20h] [ebp-74h]
    flex_t new_x; // [esp+24h] [ebp-70h]
    flex_t v158; // [esp+28h] [ebp-6Ch]
    flex_t possibly_undef_1; // [esp+2Ch] [ebp-68h]
    flex_t new_y; // [esp+30h] [ebp-64h]
    flex_t new_ya; // [esp+30h] [ebp-64h]
    rdVector3 vel_change; // [esp+34h] [ebp-60h] BYREF
    rdVector3 attachedNormal; // [esp+40h] [ebp-54h] BYREF
    rdVector3 out; // [esp+4Ch] [ebp-48h] BYREF
    rdVector3 a3; // [esp+58h] [ebp-3Ch] BYREF
    rdMatrix34 a; // [esp+64h] [ebp-30h] BYREF
    int bOverrideIdk = 0;
    flex_t zOverride = 0.0;

    possibly_undef_1 = 0.0;
    possibly_undef_2 = 0.0;

    rdVector_Zero3(&vel_change);
    v158 = 1.0;
    pThing->physicsParams.flags &= ~SITH_PF_200000;
    if ( (pThing->attach_flags & SITH_ATTACH_SURFACE) != 0 )
    {
        attachedNormal = pThing->attachedSufaceInfo->face.normal;
        possibly_undef_1 = rdMath_DistancePointToPlane(&pThing->position, &attachedNormal, &pThing->field_38);
        if ( (pThing->attachedSurface->flags & (SITH_SURFACE_ICY|SITH_SURFACE_VERYICY)) != 0 )
        {
            if ( (pThing->attachedSurface->flags & SITH_SURFACE_VERYICY) != 0 )
                possibly_undef_2 = 0.1;
            else
                possibly_undef_2 = 0.3;
        }
        else
        {
            possibly_undef_2 = 1.0;
        }
    }
    else if ( (pThing->attach_flags & SITH_ATTACH_THINGFACE) != 0 )
    {
        rdMatrix_TransformVector34(&attachedNormal, &pThing->attachedSufaceInfo->face.normal, &pThing->attachedThing->orient);
        rdMatrix_TransformVector34(&a3, &pThing->field_38, &pThing->attachedThing->orient);
        possibly_undef_2 = 1.0;
        rdVector_Add3Acc(&a3, &pThing->attachedThing->position);
        possibly_undef_1 = rdMath_DistancePointToPlane(&pThing->position, &attachedNormal, &a3);
    }

    if (pThing->physicsParams.flags & SITH_PF_800)
    {
        v158 = rdVector_Dot3(&attachedNormal, &rdroid_zVector3);
        if ( v158 < 1.0 )
            possibly_undef_1 = possibly_undef_1 / v158;
    }

    if (!(pThing->physicsParams.flags & SITH_PF_100))
    {
        if ( (pThing->physicsParams.flags & SITH_PF_ALIGNSURFACE) != 0 )
        {
            sithPhysics_SetThingLook(pThing, &attachedNormal, pThing->physicsParams.orientSpeed * secDeltaTime);
        }
        else if ( (pThing->physicsParams.flags & SITH_PF_800) != 0 )
        {
            sithPhysics_SetThingLook(pThing, &rdroid_zVector3, pThing->physicsParams.orientSpeed * secDeltaTime);
        }
        else
        {
            pThing->physicsParams.flags |= SITH_PF_100;
        }
    }

    if (pThing->physicsParams.flags & SITH_PF_USEANGULARTHRUST)
    {
        if (!rdVector_IsZero3(&pThing->physicsParams.angularVelocity))
        {
            sithPhysics_ApplyDrag(&pThing->physicsParams.angularVelocity, pThing->physicsParams.surfDrag - -0.2, 0.0, secDeltaTime);
        }

        pThing->physicsParams.angularVelocity.y = pThing->physicsParams.field_1F8.y * secDeltaTime + pThing->physicsParams.angularVelocity.y;
        rdVector_ClampValue3(&pThing->physicsParams.angularVelocity, pThing->physicsParams.maxRotationVelocity);
        rdVector_ClipPrecision3(&pThing->physicsParams.angularVelocity);
    }
    if ( pThing->physicsParams.angularVelocity.y != 0.0 )
    {
        rdVector_Scale3(&a3, &pThing->physicsParams.angularVelocity, secDeltaTime);

// MOTS added: weapon tracking?
#ifdef JKM_PARAMS
    if (pThing->type == SITH_THING_WEAPON && pThing->weaponParams.pTargetThing && pThing->weaponParams.field_38 != 0.0) {
        rdVector3 tmp;
        rdMatrix34 local_60;
        rdVector3 local_6c, local_78;

        rdVector_Sub3(&tmp, &pThing->weaponParams.pTargetThing->position, &pThing->position);
        flex_t fVar3 = secDeltaTime * pThing->weaponParams.field_38;

        if (-0.03 <= tmp.z) {
            if (tmp.z > 0.03) {
                zOverride = 1.0;
            }
        }
        else {
            zOverride = -1.0;
        }
        rdVector_Normalize3Acc(&tmp);
        rdMatrix_BuildFromLook34(&local_60,&tmp);
        rdMatrix_ExtractAngles34(&local_60,&local_6c);
        rdMatrix_Copy34(&local_60, &pThing->orient);
        rdMatrix_ExtractAngles34(&local_60,&local_78);
        tmp.y = local_6c.y - local_78.y;
        tmp.x = -local_78.x;
        tmp.z = -local_78.z;
        if (tmp.y > 180.0) {
            tmp.y = tmp.y - 360.0;
        }
        else if (tmp.y < -180.0) {
            tmp.y = tmp.y - -360.0;
        }
        flex_t fVar6 = tmp.y;
        if (tmp.y < 0.0) {
            fVar6 = -tmp.y;
        }
        if (fVar6 > fVar3) {
            fVar6 = tmp.y;
            if (tmp.y < 0.0) {
                fVar6 = -tmp.y;
            }
            tmp.y = (fVar3 / fVar6) * tmp.y;
        }
        bOverrideIdk = 1;
        rdVector_Copy3(&a3, &tmp);
    }
#endif

        rdMatrix_BuildRotate34(&a, &a3);
        sithCollision_RotateThing(pThing, &a);
        if ( possibly_undef_2 >= 1.0 )
        {
            rdMatrix_TransformVector34Acc(&pThing->physicsParams.vel, &a);
        }
        else
        {
            rdMatrix_TransformVector34(&out, &pThing->physicsParams.vel, &a);
            rdVector_Scale3Acc(&pThing->physicsParams.vel, 1.0 - possibly_undef_2);
            rdVector_ScaleAdd3Acc(&pThing->physicsParams.vel, &out, possibly_undef_2);
        }
        if ( (((jkPlayer_currentTickIdx & 0xFF) + (pThing->idx & 0xFF)) & 7) == 0 )
            rdMatrix_Normalize34(&pThing->orient);
    }
    if ( possibly_undef_2 < 0.25 )
    {
        possibly_undef_2 = 0.25;
    }
    else if ( possibly_undef_2 > 1.0 )
    {
        possibly_undef_2 = 1.0;
    }

    if (!rdVector_IsZero3(&pThing->physicsParams.vel) && pThing->physicsParams.surfDrag != 0.0)
    {
        if ( (pThing->physicsParams.flags & SITH_PF_8000) == 0 )
        {
            if ( rdVector_IsZero3(&pThing->physicsParams.acceleration)
              && !(pThing->sector->flags & SITH_SECTOR_USETHRUST)
              && possibly_undef_2 > 0.8 )
            {
                a2a = pThing->physicsParams.surfDrag * possibly_undef_2;
                v144 = pThing->physicsParams.staticDrag * possibly_undef_2;
            }
            else
            {
                a2a = pThing->physicsParams.surfDrag * possibly_undef_2;
                v144 = 0.0;
            }
            // Added: DroidWorks (DroidWorks.exe 0x45b657): a droid walking the bottom of a water
            // sector drags 1.2x (the static-drag threshold is left unscaled, as there).
            if ( Main_bDroidWorks
              && pThing->type == SITH_THING_PLAYER
              && (pThing->sector->flags & SITH_SECTOR_UNDERWATER) != 0 )
            {
                a2a *= 1.2;
            }
            sithPhysics_ApplyDrag(&pThing->physicsParams.vel, a2a, v144, secDeltaTime);
        }
        else
        {
            pThing->physicsParams.flags &= ~SITH_PF_8000;
        }
    }

    if ( (pThing->physicsParams.flags & SITH_PF_USETHRUST) != 0
      && !rdVector_IsZero3(&pThing->physicsParams.acceleration) )
    {
        flex_t v44 = possibly_undef_2 * secDeltaTime;
        // Altered: DroidWorks (DroidWorks.exe 0x45b732) has neither the crouch factor nor the
        // maxVelocity clamp on the thrust step.
        if ( (pThing->physicsParams.flags & SITH_PF_CROUCHING) != 0 && !Main_bDroidWorks )
            v44 = secDeltaTime * 0.8;
        rdVector_Scale3(&vel_change, &pThing->physicsParams.acceleration, v44);
        
        if ( !Main_bDroidWorks )
        {
            flex_t velChangeMag = rdVector_Len3(&vel_change);
            if ( pThing->physicsParams.maxVelocity < velChangeMag )
                rdVector_Scale3Acc(&vel_change, pThing->physicsParams.maxVelocity / velChangeMag);
        }
        rdVector_ClipPrecision3(&vel_change);
        if ( !rdVector_IsZero3(&vel_change) )
            rdMatrix_TransformVector34Acc(&vel_change, &pThing->orient);
    }

    if (pThing->physicsParams.mass != 0.0 && (pThing->sector->flags & SITH_SECTOR_USETHRUST) && !(pThing->physicsParams.flags & SITH_PF_NOTHRUST))
    {
        // Altered: DroidWorks (DroidWorks.exe 0x45b88b) compares the sector thrust against
        // gravity alone, not gravity * mass: its magnets push a sector up with
        // thrust 8 under gravity 4, and the baked droid weighs hundreds of units,
        // so the JK test would never lift a droid off the floor.
        flex_t liftThreshold = sithWorld_g_pCurrentWorld->gravity;
        if (!Main_bDroidWorks)
            liftThreshold *= pThing->physicsParams.mass;
        if ( pThing->sector->thrust.z > liftThreshold )
        {
            sithThing_DetachThing(pThing);
            rdVector_Zero3(&pThing->physicsParams.gravityForce);
            rdVector_Zero3(&out);
            if ( (pThing->physicsParams.flags & SITH_PF_USEANGULARTHRUST) != 0 )
            {
                if ( !rdVector_IsZero3(&pThing->physicsParams.angularVelocity) )
                {
                    sithPhysics_ApplyDrag(&pThing->physicsParams.angularVelocity, pThing->physicsParams.airDrag - -0.2, 0.0, secDeltaTime);
                }
                rdVector_ScaleAdd3Acc(&pThing->physicsParams.angularVelocity, &pThing->physicsParams.field_1F8, secDeltaTime);

                rdVector_ClampValue3(&pThing->physicsParams.angularVelocity, pThing->physicsParams.maxRotationVelocity);
                rdVector_ClipPrecision3(&pThing->physicsParams.angularVelocity);
            }
            if ( rdVector_IsZero3(&pThing->physicsParams.angularVelocity) )
            {
                rdVector_Zero3(&a3);
            }
            else
            {
                rdVector_Scale3(&a3, &pThing->physicsParams.angularVelocity, secDeltaTime);
            }
            if ( !rdVector_IsZero3(&a3) )
            {
                rdMatrix_BuildRotate34(&a, &a3);
                sithCollision_RotateThing(pThing, &a);
                if ( (pThing->physicsParams.flags & SITH_PF_FLY) != 0 )
                    rdMatrix_TransformVector34Acc(&pThing->physicsParams.vel, &a);
                if ( ((jkPlayer_currentTickIdx + (pThing->idx & 0xFF)) & 7) == 0 )
                    rdMatrix_Normalize34(&pThing->orient);
            }

            if ( pThing->physicsParams.airDrag != 0.0 )
                sithPhysics_ApplyDrag(&pThing->physicsParams.vel, pThing->physicsParams.airDrag, 0.0, secDeltaTime);

            if (pThing->physicsParams.flags & SITH_PF_USETHRUST)
            {
                if (!(pThing->physicsParams.flags & SITH_PF_FLY))
                {
                    rdVector_Scale3Acc(&pThing->physicsParams.acceleration, 0.3);
                }
                rdVector_Scale3(&out, &pThing->physicsParams.acceleration, secDeltaTime);
                // Altered: the thrust is in thing space; both JK.EXE (0x4f82cb) and
                // DroidWorks.exe (0x45bca0) rotate it by the orientation here, as the
                // non-attached path does. The decompilation had dropped this.
                rdMatrix_TransformVector34Acc(&out, &pThing->orient);
            }

            if ( pThing->physicsParams.mass != 0.0
              && (pThing->sector->flags & SITH_SECTOR_USETHRUST)
              && !(pThing->physicsParams.flags & SITH_PF_NOTHRUST))
            {
                rdVector_ScaleAdd3Acc(&out, &pThing->sector->thrust, secDeltaTime);
            }

            if ( pThing->physicsParams.mass != 0.0 && (pThing->physicsParams.flags & SITH_PF_USEGRAVITY) != 0 && (pThing->sector->flags & SITH_PF_USEGRAVITY) == 0 )
            {
                flex_t v91 = sithWorld_g_pCurrentWorld->gravity * secDeltaTime;
                if ( (pThing->physicsParams.flags & SITH_PF_PARTIALGRAVITY) != 0 )
                    v91 = v91 * 0.5;
                out.z -= v91;
                pThing->physicsParams.gravityForce.z = -v91;
            }
            rdVector_Add3Acc(&pThing->physicsParams.vel, &out);
            rdVector_ClipPrecision3(&pThing->physicsParams.vel);
            if (!rdVector_IsZero3(&pThing->physicsParams.vel))
            {
                rdVector_Scale3(&pThing->physicsParams.deltaVelocity, &pThing->physicsParams.vel, secDeltaTime);

                // Added: Turbo Mode for AI actors.
                extern int jkDev_bTurbo;

                if (!sithNet_isMulti
                    && jkDev_bTurbo
                    && pThing->type == SITH_THING_ACTOR
                    && pThing->controlType == SITH_CT_AI)
                {
                    rdVector_Scale3Acc(&pThing->physicsParams.deltaVelocity, 2.0f);
                }
            }
            return;
        }
        rdVector_ScaleAdd3Acc(&vel_change, &pThing->sector->thrust, secDeltaTime);
    }
    rdVector_Add3Acc(&pThing->physicsParams.vel, &vel_change);
    
    // Is the player climbing up/down a slope?
    if ( Main_bDroidWorks )
    {
        // Added: DroidWorks (DroidWorks.exe 0x45bf3b): the slope pull depends on the droid's
        // locomotion. Treads (dwCog_droidCaps 0x80000) feel no pull on anything gentler
        // than 75 degrees; legs get the usual fraction up to 30 degrees and FULL gravity
        // beyond it, so a legged droid slides back down a steep incline. Only applied on
        // a slope (the binary tests v158 < 1.0 strictly).
        if ( pThing->type == SITH_THING_PLAYER
          && (pThing->physicsParams.flags & SITH_PF_USEGRAVITY) != 0
          && v158 < 1.0 )
        {
            flex_t v108 = stdMath_Clamp(1.0 - possibly_undef_2, 0.2, 0.8);
            flex_t nz = rdVector_Dot3(&attachedNormal, &rdroid_zVector3);
            if ( dwCog_droidCaps & 0x80000 )
            {
                if ( nz >= 0.258819 ) // cos 75
                    v108 = 0.0;
            }
            else if ( nz < 0.866025 ) // cos 30
            {
                v108 = 1.0;
            }
            pThing->physicsParams.vel.z -= sithWorld_g_pCurrentWorld->gravity * secDeltaTime * v108;
        }
    }
    else if ( pThing->type == SITH_THING_PLAYER
      && (pThing->physicsParams.flags & SITH_PF_USEGRAVITY) != 0
      && v158 <= 1.0
      && (possibly_undef_2 < 0.8 || !rdVector_IsZero3(&pThing->physicsParams.vel)) )
    {
        flex_t v108 = stdMath_Clamp(1.0 - possibly_undef_2, 0.2, 0.8);
        pThing->physicsParams.vel.z -= sithWorld_g_pCurrentWorld->gravity * secDeltaTime * v108;
    }

    if ( !rdVector_IsZero3(&pThing->physicsParams.vel) )
    {
        flex_t v109 = rdVector_Dot3(&attachedNormal, &pThing->physicsParams.vel);

        if ( stdMath_ClipNearZero(v109) != 0.0 )
        {
            // If deltaSeconds is < the canonical phys tickrate, we get these
            // crazy oscillations
            // Altered: DroidWorks (DroidWorks.exe 0x45c05b) removes the whole normal component every
            // step; the result is the velocity's tangential part whatever the step size, so
            // the canonical-tick scaling below is not applied to it.
#ifdef FIXED_TIMESTEP_PHYS
            // Fix physics being tied to framerate?
            if (!Main_bDroidWorks && NEEDS_STEPPED_PHYS && (secDeltaTime < jkPlayer_canonicalPhysTickrate))
                v109 *= (secDeltaTime / jkPlayer_canonicalPhysTickrate);
#endif
#ifdef EXPERIMENTAL_FIXED_POINT
            if (!Main_bDroidWorks && !(NEEDS_STEPPED_PHYS) && (secDeltaTime < jkPlayer_canonicalPhysTickrate))
                v109 *= (secDeltaTime / jkPlayer_canonicalPhysTickrate);
#endif
            rdVector_ScaleAdd3Acc(&pThing->physicsParams.vel, &attachedNormal, -v109);
        }
    }

#ifdef JKM_PARAMS
    if (bOverrideIdk) {
        pThing->physicsParams.vel.z = zOverride;
    }
#endif

    rdVector_ClipPrecision3(&pThing->physicsParams.vel);
    if (!rdVector_IsZero3(&pThing->physicsParams.vel))
    {
        rdVector_Scale3(&pThing->physicsParams.deltaVelocity, &pThing->physicsParams.vel, secDeltaTime);

        // Added: Turbo Mode for AI actors.
        extern int jkDev_bTurbo;

        if (!sithNet_isMulti
            && jkDev_bTurbo
            && pThing->type == SITH_THING_ACTOR
            && pThing->controlType == SITH_CT_AI)
        {
            rdVector_Scale3Acc(&pThing->physicsParams.deltaVelocity, 2.0f);
        }
    }

    flex_t v131;
    // Altered: DroidWorks (DroidWorks.exe 0x45c206) has no crouch variant of the floor distance.
    if ( (pThing->physicsParams.flags & SITH_PF_CROUCHING) && !Main_bDroidWorks )
    {
        v131 = v158 * possibly_undef_1 - (pThing->moveSize - -0.01);
    }
    else
    {
        flex_t v132 = pThing->physicsParams.height;
        if ( v132 == 0.0 )
        {
            if ( pThing->renderData.type == RD_THING_MODEL3 )
                v132 = pThing->renderData.model3->insertOffset.z;
            new_ya = pThing->moveSize - -0.005;
            if ( v132 <= new_ya )
                v132 = new_ya;
        }
        v131 = possibly_undef_1 - v132;
    }

    // Slide down slopes
    v131 = stdMath_ClipNearZero(v131);
    {
        // Added: DW_DEBUG_COLL headless physics trace (see sithCollision_MoveThing)
        if ( sithCollision_bDwDebugColl > 0 && pThing == sithPlayer_g_pLocalPlayerThing )
            stdPlatform_Printf("PHYS: dt %.4f pu1 %.5f h %.5f v131 %.5f v158 %.3f n %.3f %.3f %.3f vel %.4f %.4f %.4f dvel %.5f %.5f %.5f att 0x%x surf %d\n",
                secDeltaTime, possibly_undef_1, sithPhysics_GetThingHeight(pThing), v131, v158,
                attachedNormal.x, attachedNormal.y, attachedNormal.z,
                pThing->physicsParams.vel.x, pThing->physicsParams.vel.y, pThing->physicsParams.vel.z,
                pThing->physicsParams.deltaVelocity.x, pThing->physicsParams.deltaVelocity.y, pThing->physicsParams.deltaVelocity.z,
                pThing->attach_flags, (pThing->attach_flags & SITH_ATTACH_SURFACE) ? (int)(pThing->attachedSurface - sithWorld_g_pCurrentWorld->surfaces) : -1);
    }
    if ( v131 != 0.0 )
    {
        // Fix physics being tied to framerate?
        flex_t orig_v131 = stdMath_ClampValue(v131, secDeltaTime * 0.5);
        flex_t new_v131 = v131 * (secDeltaTime / jkPlayer_canonicalPhysTickrate);
        new_v131 = stdMath_ClampValue(new_v131, secDeltaTime * 0.5);

        // If deltaSeconds is < the canonical phys tickrate, we get these
        // crazy oscillations
        // Note: DroidWorks (DroidWorks.exe 0x45c24a) closes the whole gap every frame (limited to
        // 0.5 units/s). That only lets a droid onto a ramp because a 15 fps frame carries it
        // across the ~r/4 zone where its sphere already presses on the ramp while it is still
        // attached to the flat floor; at small steps (the original above ~30 fps included) the
        // full correction pulls it back exactly as fast as the ramp slides it up and it locks
        // at the ramp base. The canonical 1/15 s lag below keeps the correction's settling
        // time at the original's frame length whatever the step, so it stays.
#ifdef FIXED_TIMESTEP_PHYS
        if (NEEDS_STEPPED_PHYS && secDeltaTime < jkPlayer_canonicalPhysTickrate)
            v131 = new_v131;
        else
            v131 = orig_v131;
#else
        v131 = orig_v131;
#endif
#ifdef EXPERIMENTAL_FIXED_POINT
        if (secDeltaTime < jkPlayer_canonicalPhysTickrate)
            v131 = new_v131;
#endif

        // Added: Fix turret slowly drifting up?
        if ((pThing->type == SITH_THING_ACTOR || pThing->type == SITH_THING_PLAYER) 
            && (pThing->actorParams.flags & SITH_AF_COMBO_FREEZE))
        {
            v131 = orig_v131;
        }

        // Altered: DroidWorks (DroidWorks.exe 0x45c289) always corrects along the attached
        // surface's normal; the vertical variant for SITH_PF_800 things is JK-only.
        if ( (pThing->physicsParams.flags & SITH_PF_800) != 0 && !Main_bDroidWorks )
        {
            rdVector_ScaleAdd3Acc(&pThing->physicsParams.deltaVelocity, &rdroid_zVector3, -v131);
        }
        else
        {
            rdVector_ScaleAdd3Acc(&pThing->physicsParams.deltaVelocity, &attachedNormal, -v131);
        }
    }
}