// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoopNetwork.h"
#include "Player.h"
#include "PlayerHelper.h"
#include "PlayerHands.h"
#include "MapHandler.h"
#include "GameEnemy.h"
#include "Inventory.h"
#include "GameItemType.h"
#include "GameMessageHandler.h"
#include "SaveHandler.h"
#include "Notebook.h"
#include "FadeHandler.h"
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <windows.h>
using namespace hpl;
namespace {
struct CampaignScope {
    cInit* i; cPlayer* p; cPlayerHands* h;
    CampaignScope(cInit* init,cPlayer* player,cPlayerHands* hands):i(init),p(init->mpPlayer),h(init->mpPlayerHands) { i->mpPlayer=player; i->mpPlayerHands=hands; }
    ~CampaignScope() { i->mpPlayer=p; i->mpPlayerHands=h; }
};
}
void cCoopNetwork::RunCampaignQA(float dt) {
    const bool resume=std::string(getenv("PENUMBRA_COOP_QA"))=="resume";
    if(!Ready() && campaignTime==0) return;
    campaignTime+=dt;
    auto* world=init->mpGame->GetScene()->GetWorld3D(); if(!world) return;
    auto* local=init->mpPlayer;
    init->mpGameMessageHandler->Reset();
    auto entity=[&](const std::string& name) { return init->mpMapHandler->GetGameEntity(name,false); };
    auto enemy=[&](const std::string& name) { auto* e=entity(name); return e && e->GetType()==eGameEntityType_Enemy?static_cast<iGameEnemy*>(e):nullptr; };
    auto spawnEnemy=[&](const std::string& name,const std::string& file) {
        auto frame=cMath::MatrixTranslate(cVector3f(0,.3f,1.5f));
        world->CreateEntity(name,frame,file,true);
        auto* e=enemy(name); if(!e) FatalError("[COOP/QA] Enemy fixture missing %s\n",name.c_str());
        if(world->GetAINodeList("ground")->empty()) for(int n=0;n<24;++n) world->AddAINode("CoopQA_Node"+std::to_string(n),"ground",cVector3f((n%3-1)*.6f,.3f,2.0f+(n/3)*.7f));
        e->OnWorldLoad(); e->SetActive(true); e->GetMover()->GetCharBody()->SetFeetPosition(cVector3f(0,.3f,1.5f));
        e->GetMover()->GetCharBody()->SetYaw(kPif); return e;
    };
    auto savedState=[&]() {
        cNotebook_GlobalSave book; init->mpNotebook->SaveToGlobal(&book);
        bool task=false; auto tasks=book.mlstTasks.GetIterator(); while(tasks.HasNext()) if(tasks.Next().msName=="CoopQAProgress") task=true;
        auto* crate=entity("CoopQA_SaveCrate"); auto* parts=entity("CoopQA_SaveCrate_broken");
        auto* dog=enemy("CoopQA_DogHost");
        auto* progress=init->mpGame->GetScene()->GetGlobalVar("CoopQAProgress");
        auto* deadDog=enemy("CoopQA_DogClient"); const bool corpse=deadDog && deadDog->GetHealth()<=0 && deadDog->GetMeshEntity()->GetSkeletonPhysicsActive();
        const bool worldOK=corpse && (!crate || !crate->IsActive()) && parts && parts->GetBodyNum()==10 && dog && !dog->IsActive() && std::abs(dog->GetHealth()-25)<1;
        const float expected=IsHost()?67:43;
        const bool selfOK=local->GetHealth()>=expected-.1f && local->GetHealth()<expected+15 && std::abs(local->GetPower()-(IsHost()?73:61))<1.5f;
        const bool items=init->mpInventory->GetItem("hammer") && local->GetState()==ePlayerState_WeaponMelee && local->GetFlashLight()->IsActive();
        const bool otherOK=IsClient() || (remotePlayer->GetHealth()>=42.9f && remotePlayer->GetHealth()<58 && std::abs(remotePlayer->GetPower()-61)<1.5f && remotePlayer->GetState()==ePlayerState_WeaponMelee && remotePlayer->GetFlashLight()->IsActive());
        const bool ok=worldOK && selfOK && items && otherOK && task && (IsClient() || (progress && progress->mlVal==31415));
        Log("[COOP/QA] Saved state role=%s world=%d self=%d items=%d partner=%d notebook=%d health=%.2f power=%.2f epoch=%u\n",IsHost()?"host":"client",worldOK,selfOK,items,otherOK,task,local->GetHealth(),local->GetPower(),g->map.epoch);
        Log("[COOP/QA] Equipment role=%s state=%d lamp=%d partner_state=%d partner_lamp=%d tool=%d flags=%d\n",IsHost()?"host":"client",int(local->GetState()),local->GetFlashLight()->IsActive(),remotePlayer?int(remotePlayer->GetState()):-1,remotePlayer?remotePlayer->GetFlashLight()->IsActive():0,g->latest.players[1].tinderboxes,g->latest.players[1].flags); return ok;
    };
    if(resume) {
        if(campaignTime>3 && campaignStage==0) { qaLoaded=savedState(); campaignStage=1; }
        if(campaignTime>(IsHost()?7:5)) {
            Log("[COOP/QA] %s fresh process checkpoint, both players, inventory, notebook and broken world role=%s\n",qaLoaded?"PASS":"FAIL",IsHost()?"host":"client"); init->mpGame->Exit();
        }
        return;
    }
    if(IsClient()) {
        if(local->GetHealth()<95 && !local->IsDead()) qaClientAttacked=true;
        if(g->latest.players[0].health<9500 && !(g->latest.players[0].flags&8)) qaHostAttacked=true;
        auto* dog=enemy("CoopQA_DogClient");
        if(dog && dog->GetMeshEntity()->GetAnimationStateNum()>0 && dog->GetHealth()>0) {
            for(int n=0;n<dog->GetMeshEntity()->GetAnimationStateNum();++n) {
                auto* a=dog->GetMeshEntity()->GetAnimationState(n); if(a->IsActive() && a->GetTimePosition()>.05f) qaEnemySeen=true;
            }
        }
        if(local->IsDead()) qaRespawned=true;
        // Tempi agganciati al primo ricaricamento del checkpoint (nuova epoca),
        // non all'avvio: la durata delle fasi dell'host varia. Dopo ~9 s l'host
        // fa morire tutti e ricarica di nuovo nella stessa epoca; il controllo
        // segue quello, e il client resta connesso per il controllo finale
        // dell'host (~21 s dopo il ricaricamento).
        if(!qaStartEpoch) qaStartEpoch=g->map.epoch;
        if(g->map.epoch>qaStartEpoch) qaReloadWait+=dt;
        if(campaignStage==0 && Ready() && qaReloadWait>15) { qaLoaded=savedState(); campaignStage=1; }
        if(qaReloadWait>28 || campaignTime>140) {
            const bool ok=qaClientAttacked && qaHostAttacked && qaEnemySeen && qaRespawned && qaLoaded;
            Log("[COOP/QA] %s enemy replica, both damaged, death and checkpoint baseline role=client\n",ok?"PASS":"FAIL"); init->mpGame->Exit();
        }
        return;
    }
    if(campaignStage==0) {
        local->StartHolster(); remotePlayer->ChangeState(ePlayerState_Normal);
        local->ChangeMoveState(ePlayerMoveState_Walk,true); remotePlayer->ChangeMoveState(ePlayerMoveState_Walk,true);
        if(auto* crate=entity("CoopQA_ClientCrate")) crate->SetActive(false);
        if(auto* crate=entity("CoopQA_HostCrate")) crate->SetActive(false);
        local->GetFlashLight()->SetActive(false);
        { CampaignScope scope(init,remotePlayer,remoteHands); remotePlayer->GetFlashLight()->SetActive(false); }
        local->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,2.3f));
        remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,10.3f));
        local->SetHealth(100); remotePlayer->SetHealth(100);
        spawnEnemy("CoopQA_DogHost","character_dog.ent"); campaignStage=1;
    }
    if(campaignStage==1) { auto* dog=enemy("CoopQA_DogHost"); qaHostAttacked=qaHostAttacked || (dog && dog->GetNetworkTarget()==local && local->GetHealth()<99); }
    if(campaignStage==2) { auto* dog=enemy("CoopQA_DogClient"); qaClientAttacked=qaClientAttacked || (dog && dog->GetNetworkTarget()==remotePlayer && remotePlayer->GetHealth()<99); }
    if(campaignStage==1 && campaignTime>9) {
        auto* dog=enemy("CoopQA_DogHost");
        /* Damage was sampled during the native attack, including a possible death. */
        Log("[COOP/QA] Native dog attacked host=%d health=%.2f state=%d enemy_pos=%s player_pos=%s\n",qaHostAttacked,local->GetHealth(),dog?dog->GetCurrentStateId():-1,dog?dog->GetPosition().ToString().c_str():"none",local->GetCharacterBody()->GetPosition().ToString().c_str());
        if(dog) dog->SetActive(false);
        local->SetHealth(100); local->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,10.3f));
        remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,10.3f));
        remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,2.3f));
        spawnEnemy("CoopQA_DogClient","character_dog.ent"); campaignStage=2;
    }
    if(campaignStage==2 && campaignTime>19) {
        auto* dog=enemy("CoopQA_DogClient");
        /* Preserve the observed native damage even if an individual respawn completed. */
        Log("[COOP/QA] Native dog attacked client=%d health=%.2f target=%d\n",qaClientAttacked,remotePlayer->GetHealth(),dog && dog->GetNetworkTarget()==remotePlayer?2:1);
        if(dog) dog->SetActive(false);
        local->GetCharacterBody()->SetForce(cVector3f(0)); local->GetCharacterBody()->SetForceVelocity(cVector3f(0));
        remotePlayer->GetCharacterBody()->SetForce(cVector3f(0)); remotePlayer->GetCharacterBody()->SetForceVelocity(cVector3f(0));
        local->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,2.3f)); local->SetHealth(100); local->SetActive(true);
        DamagePlayer(remotePlayer,200); campaignStage=3;
    }
    if(campaignStage==3 && campaignTime>26) {
        qaRespawned=!remotePlayer->IsDead() && remotePlayer->GetHealth()==100 && (remotePlayer->GetCharacterBody()->GetFeetPosition()-local->GetCharacterBody()->GetFeetPosition()).Length()<3;
        Log("[COOP/QA] Client respawn near surviving host=%d\n",qaRespawned);
        DamagePlayer(local,200); campaignStage=4;
    }
    if(campaignStage==4 && campaignTime>33) {
        const bool hostBack=!local->IsDead() && local->GetHealth()==100;
        qaRespawned=qaRespawned && hostBack;
        Log("[COOP/QA] Host respawn while client survives=%d\n",hostBack);
        local->SetHealth(100); local->SetActive(true); local->GetCharacterBody()->SetForceVelocity(cVector3f(0)); local->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,10.3f)); remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,10.3f));
        remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,2.3f));
        spawnEnemy("CoopQA_Spider","character_spider.ent"); campaignStage=5;
    }
    // Il morso si registra durante l'attacco, come per i cani: se il ragno
    // uccide il client prima della fine della fase passa giustamente
    // all'host, e un controllo solo finale lo darebbe per mancato.
    if(campaignStage==5) { auto* spider=enemy("CoopQA_Spider"); qaSpiderHit=qaSpiderHit || (spider && spider->GetNetworkTarget()==remotePlayer && remotePlayer->GetHealth()<99); }
    if(campaignStage==5 && campaignTime>43) {
        auto* spider=enemy("CoopQA_Spider");
        const bool hit=qaSpiderHit;
        qaClientAttacked=qaClientAttacked && hit;
        Log("[COOP/QA] Native spider attacked client=%d health=%.2f\n",hit,remotePlayer->GetHealth());
        if(spider) spider->SetActive(false);
        remotePlayer->SetHealth(100);
        auto* worm=spawnEnemy("CoopQA_Worm","character_rockworm.ent");
        worm->GetMover()->GetCharBody()->SetFeetPosition(cVector3f(0,.3f,.9f)); campaignStage=6;
    }
    if(campaignStage==6) { auto* worm=enemy("CoopQA_Worm"); qaEnemySeen=qaEnemySeen || (worm && worm->GetNetworkTarget()==remotePlayer && remotePlayer->GetHealth()<99); }
    if(campaignStage==6 && campaignTime>55) {
        auto* worm=enemy("CoopQA_Worm");
        const bool target=worm && worm->GetNetworkTarget()==remotePlayer;
        const bool wormHit=qaEnemySeen;
        Log("[COOP/QA] Native worm attacked client=%d final_target=%d health=%.2f\n",wormHit,target?2:1,remotePlayer->GetHealth());
        if(worm) worm->SetActive(false);
        if(auto* dog=enemy("CoopQA_DogClient")) { dog->SetActive(true); dog->Damage(1000,10); Log("[COOP/QA] Native lethal enemy damage health=%.2f ragdoll=%d\n",dog->GetHealth(),dog->GetMeshEntity()->GetSkeletonPhysicsActive()); }
        local->SetHealth(67); remotePlayer->SetHealth(43); local->SetPower(73); remotePlayer->SetPower(61);
        local->SetActive(true); local->GetCharacterBody()->SetForce(cVector3f(0)); local->GetCharacterBody()->SetForceVelocity(cVector3f(0)); remotePlayer->GetCharacterBody()->SetForce(cVector3f(0)); remotePlayer->GetCharacterBody()->SetForceVelocity(cVector3f(0)); local->GetCharacterBody()->SetFeetPosition(cVector3f(0,.3f,2.3f)); remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(.6f,.3f,2.3f));
        auto* item=init->mpInventory->GetItem("hammer");
        if(item) {
            init->mpInventory->GetItemType(item->GetItemType())->OnAction(item,0);
            CampaignScope scope(init,remotePlayer,remoteHands); init->mpInventory->GetItemType(item->GetItemType())->OnAction(item,0);
        }
        local->GetFlashLight()->SetActive(true);
        { CampaignScope scope(init,remotePlayer,remoteHands); remotePlayer->GetFlashLight()->SetActive(true); }
        auto frame=cMath::MatrixTranslate(cVector3f(0,.55f,4.8f)); world->CreateEntity("CoopQA_SaveCrate",frame,"modern_mine_woodcrate.ent",true);
        if(auto* crate=entity("CoopQA_SaveCrate")) crate->Damage(200,10);
        init->mpMapHandler->Update(0);
        if(auto* dog=enemy("CoopQA_DogHost")) dog->SetHealth(25);
        init->mpNotebook->AddTask("CoopQAProgress",L"A cooperative checkpoint must retain this task.");
        init->mpGame->GetScene()->CreateGlobalVar("CoopQAProgress")->mlVal=31415;
        { CampaignScope scope(init,remotePlayer,remoteHands); qaSaved=init->mpSaveHandler->SaveCheckpoint(); }
        Log("[COOP/QA] Complete native checkpoint saved=%d\n",qaSaved); campaignStage=7;
    }
    if(campaignStage==7 && campaignTime>60) {
        const auto before=local->GetHealth();
        std::ofstream invalid(cString::To8Char(init->mpSaveHandler->GetSaveDir()+L"save/invalid.sav")); invalid<<"<SaveGame/>"; invalid.close();
        init->mpSaveHandler->LoadGameFromFile(L"save/invalid.sav");
        bool safe=!init->mpSaveHandler->LastOperationOK() && local->GetHealth()==before;
        const auto checkpoint=init->mpSaveHandler->GetSaveDir()+L"save/coop_checkpoint.sav";
        auto read=[](const tWString& path) { std::ifstream f(cString::To8Char(path),std::ios::binary); return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()); };
        const auto original=read(checkpoint); auto tampered=original;
        const auto mark=tampered.find("integrity=\"");
        if(mark==std::string::npos) FatalError("[COOP/QA] Checkpoint lacks integrity field\n");
        tampered[mark+11]=tampered[mark+11]=='0'?'1':'0';
        std::ofstream corrupt(cString::To8Char(init->mpSaveHandler->GetSaveDir()+L"save/tampered.sav"),std::ios::binary); corrupt<<tampered; corrupt.close();
        init->mpSaveHandler->LoadGameFromFile(L"save/tampered.sav");
        const bool tamperSafe=!init->mpSaveHandler->LastOperationOK() && local->GetHealth()==before;
        HANDLE lock=CreateFileW((checkpoint+L".tmp").c_str(),GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(lock==INVALID_HANDLE_VALUE) FatalError("[COOP/QA] Cannot lock temporary checkpoint\n");
        const bool failedWrite=!init->mpSaveHandler->SaveCheckpoint(); CloseHandle(lock);
        const bool writeSafe=failedWrite && read(checkpoint)==original;
        Log("[COOP/QA] Corrupted XML rejected=%d failed atomic write preserved checkpoint=%d\n",tamperSafe,writeSafe);
        safe=safe && tamperSafe && writeSafe;
        Log("[COOP/QA] Invalid checkpoint preserves running world=%d\n",safe);
        local->SetHealth(3); remotePlayer->SetHealth(5); local->SetPower(1); remotePlayer->SetPower(2);
        if(auto* hammer=init->mpInventory->GetItem("hammer")) init->mpInventory->RemoveItem(hammer);
        qaLoaded=init->mpSaveHandler->LoadCheckpoint() && safe; campaignStage=8;
    }
    if(campaignStage==8 && campaignTime>69 && Ready()) {
        qaLoaded=qaLoaded && savedState();
        DamagePlayer(local,200); DamagePlayer(remotePlayer,200); campaignStage=9;
    }
    if(campaignStage==9 && campaignTime>83 && Ready()) { qaLoaded=qaLoaded && savedState(); campaignStage=10; }
    if(campaignTime>95) {
        const bool ok=qaHostAttacked && qaClientAttacked && qaEnemySeen && qaRespawned && qaSaved && qaLoaded && campaignStage==10;
        Log("[COOP/QA] %s native enemies, both victims, respawn, atomic save and automatic checkpoint reload role=host\n",ok?"PASS":"FAIL"); init->mpGame->Exit();
    }
}
