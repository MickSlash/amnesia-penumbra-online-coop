// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoopNetwork.h"
#include "Player.h"
#include "SaveHandler.h"
#include "Notebook.h"
#include "GameEnemy.h"
#include "PlayerHelper.h"
#include "PlayerHands.h"
#include "MapHandler.h"
#include "GameEntity.h"
#include "Inventory.h"
#include "GameItemType.h"
#include "GameMessageHandler.h"
#include "FadeHandler.h"
#include "MainMenu.h"
#include "EffectHandler.h"
#include "GameMusicHandler.h"
#include "HudModel_Weapon.h"
#include "PlayerState_Weapon.h"
#include "graphics/Renderer3D.h"
#include "resources/TextureManager.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <thread>

using namespace hpl;
namespace hpl { extern bool gbGameScriptExecution; }
namespace hpl { extern bool (*gGameGuiSoundCallback)(const tString&,float); }
namespace {
cCoopNetwork* activeNetwork=nullptr;
uint64_t Milliseconds() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
bool ScriptSound(const tString& name,float volume) {
    if(!activeNetwork) return false;
    activeNetwork->RelayGuiSound(name,volume);
    return activeNetwork->IsHost() && activeNetwork->IsRemoteContext();
}
// Replays the client's unacknowledged commands with its own iCharacterBody, so
// collisions, sliding along walls and gradual step climbing match the host's
// simulation of the same commands. The body runs in prediction mode (no
// callbacks, camera or side effects) and is restored afterwards.
struct NativeStepper {
    iCharacterBody* body; iCharacterBody::tPredictionState saved;
    explicit NativeStepper(iCharacterBody* b) : body(b) {}
    void Begin(const online::PlayerState& s) {
        saved=body->GetPredictionState();
        auto start=saved;
        start.mvPosition=start.mvLastPosition=cVector3f(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f)+cVector3f(0,body->GetShape()->GetSize().y/2,0);
        start.mvVelocity=cVector3f(0,0,0); start.mvForce=cVector3f(0,0,0);
        start.mfMoveSpeed[eCharDir_Forward]=s.forwardSpeed/1000.0f; start.mfMoveSpeed[eCharDir_Right]=s.sideSpeed/1000.0f;
        start.mbMoving[0]=start.mbMoving[1]=false; start.mfCheckStepClimbCount=0; start.mbOnGround=true; start.mbClimbing=false;
        body->SetPredictionState(start); body->SetPredicting(true);
    }
    online::PredictedPosition Step(const online::PlayerInput& input, float dt) {
        body->SetYaw(input.yaw/1000.0f);
        if(input.forward) body->Move(eCharDir_Forward,input.forward/1000.0f,dt);
        if(input.strafe) body->Move(eCharDir_Right,input.strafe/1000.0f,dt);
        body->Update(dt);
        const cVector3f position=body->GetPosition(); return {position.x,position.z};
    }
    void End() { body->SetPredicting(false); body->SetPredictionState(saved); }
};
bool KinematicPrediction() { const char* mode=getenv("PENUMBRA_COOP_PREDICTION"); return mode && std::string(mode)=="kinematic"; }
struct PlayerScope {
    cInit* init; cPlayer* player; cPlayerHands* hands; bool simple;
    PlayerScope(cInit* i, cPlayer* p, cPlayerHands* h) : init(i), player(i->mpPlayer), hands(i->mpPlayerHands), simple(i->mbSimpleWeaponSwing) {
        init->mpPlayer = p; init->mpPlayerHands = h;
        if(p && p->IsNetworkRemote() && i->mpCoop) init->mbSimpleWeaponSwing=i->mpCoop->RemoteSimpleSwing();
    }
    ~PlayerScope() { init->mpPlayer = player; init->mpPlayerHands = hands; init->mbSimpleWeaponSwing=simple; }
};
float Wrapped(float angle) { while(angle > kPif) angle -= k2Pif; while(angle < -kPif) angle += k2Pif; return angle; }
cVector3f AimAngles(const cVector3f& from,const cVector3f& target) {
    auto angles=cMath::GetAngleFromPoints3D(from,target);
    // The engine helper returns pitch in [0,2*pi); the camera clamps signed pitch.
    angles.x=Wrapped(angles.x); angles.y=Wrapped(angles.y); return angles;
}
online::Transform Pack(const cMatrixf& m) {
    online::Transform out;
    for(int r=0;r<3;++r) for(int c=0;c<4;++c) out[r*4+c]=int32_t(std::round(m.m[r][c]*(c==3?1000:10000)));
    return out;
}
cMatrixf Unpack(const online::Transform& in) {
    cMatrixf out=cMatrixf::Identity;
    for(int r=0;r<3;++r) for(int c=0;c<4;++c) out.m[r][c]=in[r*4+c]/(c==3?1000.0f:10000.0f);
    return out;
}
uint32_t EntityID(const std::string& s) { uint32_t h=2166136261u; for(unsigned char c:s) h=(h^c)*16777619u; return h?h:1; }
uint64_t Fingerprint(const std::string& path) {
    std::ifstream file(path,std::ios::binary); if(!file) return 0;
    uint64_t hash=14695981039346656037ull; char block[65536];
    while(file) { file.read(block,sizeof block); for(std::streamsize i=0;i<file.gcount();++i) hash=(hash^uint8_t(block[i]))*1099511628211ull; }
    return hash;
}
online::PlayerState Capture(cPlayer* p,cPlayerHands* hands) {
    online::PlayerState s; auto* body=p->GetCharacterBody(); auto* camera=p->GetCamera();
    cVector3f pos=body->GetFeetPosition(); s.x=int32_t(std::round(pos.x*1000)); s.y=int32_t(std::round(pos.y*1000)); s.z=int32_t(std::round(pos.z*1000));
    s.yaw=int16_t(Wrapped(camera->GetYaw())*1000); s.pitch=int16_t(camera->GetPitch()*1000);
    s.health=uint16_t(std::clamp(p->GetHealth(),0.0f,100.0f)*100); s.oil=uint16_t(std::clamp(p->GetPower(),0.0f,100.0f)*100);
    s.eyeHeight=int16_t((camera->GetPosition().y-pos.y)*1000);
    s.flags=(p->IsActive()?4:0)|(p->IsDead()?8:0)|(p->GetMoveState()==ePlayerMoveState_Crouch?1:0)|(p->GetFlashLight()->IsActive()?2:0);
    if(p->GetState()==ePlayerState_Move) s.flags|=16;
    if(p->GetState()==ePlayerState_WeaponMelee && hands->GetCurrentModel(1)) {
        auto* weapon=hands->GetCurrentModel(1);
        s.tinderboxes=cString::ToLowerCase(weapon->msName)=="hammer"?1:2;
        if(static_cast<cHudModel_WeaponMelee*>(weapon)->IsAttacking()) s.flags|=64;
    }
    s.forwardSpeed=int16_t(body->GetMoveSpeed(eCharDir_Forward)*1000); s.sideSpeed=int16_t(body->GetMoveSpeed(eCharDir_Right)*1000);
    s.forwardMax=uint16_t(body->GetMaxPositiveMoveSpeed(eCharDir_Forward)*1000); s.sideMax=uint16_t(body->GetMaxPositiveMoveSpeed(eCharDir_Right)*1000);
    s.backwardMax=uint16_t(-body->GetMaxNegativeMoveSpeed(eCharDir_Forward)*1000);
    s.acceleration=uint16_t(body->GetMoveAcc(eCharDir_Forward)*1000); s.deceleration=uint16_t(body->GetMoveDeacc(eCharDir_Forward)*1000);
    return s;
}
}

cCoopNetwork::cCoopNetwork(cInit* i) : iUpdateable("OvertureOnline"), init(i), session([](const std::string& s){ Log("[COOP] %s\n",s.c_str()); }) {
    hostPlayer=init->mpPlayer; hostHands=init->mpPlayerHands;
    const char* role=getenv("PENUMBRA_COOP_ROLE"); if(!role) return;
    const char* portText=getenv("PENUMBRA_COOP_PORT"); const long port=portText?std::strtol(portText,nullptr,10):online::DefaultPort;
    if(port<1 || port>65535) FatalError("Invalid cooperative UDP port\n");
    if(std::string(role)=="host") { if(!session.StartHost(uint16_t(port))) FatalError("Cannot open cooperative UDP port\n"); CreateRemote(); }
    else if(std::string(role)=="client") {
        const char* address=getenv("PENUMBRA_COOP_ADDRESS");
        if(!address || !*address || !session.StartClient(address,uint16_t(port))) FatalError("Cannot connect to cooperative host\n");
        hpl::gbGameScriptExecution=false;
    } else FatalError("Invalid cooperative role\n");
    Log("[COOP] Experimental Overture protocol %u; campaign authority=host\n",unsigned(online::ProtocolVersion));
    if(IsHost()) cSoundEntity::AddGlobalCallback(this);
    activeNetwork=this; hpl::gGameGuiSoundCallback=ScriptSound;
    steam.Start();
}
cCoopNetwork::~cCoopNetwork() {
    hpl::gGameGuiSoundCallback=nullptr; activeNetwork=nullptr;
    cSoundEntity::RemoveGlobalCallback(this);
    session.CloseVoluntarily(); session.Stop();
    if(remotePlayer) {
        PlayerScope scope(init,remotePlayer,remoteHands);
        if(remotePlayer->GetCharacterBody()) remotePlayer->OnWorldExit();
        remoteHands->OnWorldExit();
        delete remotePlayer; delete remoteHands;
    }
    hpl::gbGameScriptExecution=true;
}
void cCoopNetwork::CreateRemote() {
    remoteHands=new cPlayerHands(init);
    { PlayerScope scope(init,init->mpPlayer,remoteHands); remotePlayer=new cPlayer(init); remotePlayer->SetNetworkRemote(true); }
    init->mpGame->GetScene()->SetCamera(init->mpPlayer->GetCamera());
}
bool cCoopNetwork::Ready() const { return map.epoch && session.Connected() && (IsHost()?session.IsWorldApplied(map.epoch):positioned); }
void cCoopNetwork::BeforeMapLoad() {
    if(IsHost()) { PrepareSave(); session.BeginMapLoad(); }
    if(remotePlayer && remotePlayer->GetCharacterBody()) { PlayerScope scope(init,remotePlayer,remoteHands); remotePlayer->OnWorldExit(); remoteHands->OnWorldExit(); }
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    if(world) {
        for(auto* part:avatar) if(part) world->DestroyMeshEntity(part);
        if(remoteLight) world->DestroyLight(remoteLight);
        if(remoteTool) world->DestroyMeshEntity(remoteTool);
        if(remoteFlashlight) world->DestroyMeshEntity(remoteFlashlight);
    }
    for(auto& part:avatar) part=nullptr;
    remoteLight=nullptr; hasAvatarPosition=false; positioned=false; baselineSent=false;
    remoteTool=nullptr; remoteToolID=0; toolSwing=0; hasClientPosition=false; snapshotAge=0;
    remoteFlashlight=nullptr;
    prediction.Clear(); motion.clear(); published.clear(); pendingPresentation.clear();
    restoredEquipment=false; baselineNames.clear(); journalBytes.clear(); receivedJournal=0; remoteButtons=0; lastInput=0;
    remoteExamineDown=false;
    mediaBytes.clear(); pendingMedia.clear(); currentMusic.clear(); currentSkybox.clear(); inventoryBytes.clear();
}
void cCoopNetwork::AfterMapLoad(const tString& file) {
    if(!IsOnline()) return;
    if(IsHost()) {
        ++map.epoch; map.file="maps/"+cString::GetFileName(file); map.fingerprint=Fingerprint(map.file);
        if(!map.fingerprint) FatalError("Cannot fingerprint cooperative map '%s'\n",map.file.c_str());
        { PlayerScope scope(init,remotePlayer,remoteHands); remotePlayer->OnWorldLoad(); remoteHands->OnWorldLoad(); if(map.epoch==1) remotePlayer->SetPower(100); }
        spawn=init->mpPlayer->GetCharacterBody()->GetFeetPosition();
        remotePlayer->GetCharacterBody()->SetFeetPosition(spawn+cVector3f(0.6f,0,0));
        remotePlayer->GetCharacterBody()->SetCollideCharacter(false); init->mpPlayer->GetCharacterBody()->SetCollideCharacter(false);
        remotePlayer->GetCamera()->SetYaw(init->mpPlayer->GetCamera()->GetYaw());
        remotePlayer->GetCamera()->SetPitch(init->mpPlayer->GetCamera()->GetPitch());
        remotePlayer->GetCharacterBody()->SetYaw(remotePlayer->GetCamera()->GetYaw());
        if(session.Connected()) session.SendMap(map);
    } else {
        init->mpPlayer->SetActive(false); init->mpFadeHandler->FadeIn(0); session.SendMapReady(map.epoch);
    }
    Log("[COOP/MAP] Loaded epoch=%u map=%s role=%s\n",map.epoch,file.c_str(),IsHost()?"host":"client");
}
bool cCoopNetwork::ClientAction(online::PlayerAction action, bool pressed,const std::string& item,const std::string& second) {
    if(!IsClient()) return false;
    if(action==online::PlayerAction::Interact) { interactHeld=pressed; if(!pressed) gestureYaw=gesturePitch=0; }
    online::ActionRequest request; request.epoch=map.epoch; request.sequence=++actionSequence; request.action=action; request.pressed=pressed; request.item=item; request.secondItem=second;
    if(action==online::PlayerAction::Interact && pressed && init->mpPlayer->GetState()==ePlayerState_UseItem && init->mpPlayer->GetCurrentItem())
        request.item=init->mpPlayer->GetCurrentItem()->GetName();
    if(Ready()) session.SendAction(request);
    if(action==online::PlayerAction::Interact && pressed && !request.item.empty()) init->mpPlayer->ChangeState(ePlayerState_Normal);
    if(action==online::PlayerAction::Interact && pressed && init->mpGameMessageHandler->HasMessage()) init->mpGameMessageHandler->ShowNext();
    // Predict the first-person swing immediately; world damage runs only on the host.
    return !(action==online::PlayerAction::Interact && init->mpPlayer->GetState()==ePlayerState_WeaponMelee);
}
void cCoopNetwork::ClientWeaponModel(const std::string& model) {
    if(!IsClient() || !Ready() || applyingBaseline || !restoredEquipment) return;
    if(model.empty()) { ClientAction(online::PlayerAction::UseInventory,false); return; }
    for(const auto& pair:init->mpInventory->NetworkItems()) {
        auto* item=pair.second;
        if(item->GetItemType()==eGameItemType_WeaponMelee && cString::ToLowerCase(item->GetHudModelName())==cString::ToLowerCase(model)) {
            ClientAction(online::PlayerAction::UseInventory,true,item->GetName(),init->mbSimpleWeaponSwing?"simple":"manual"); return;
        }
    }
}
void cCoopNetwork::UpdateEnemy(iGameEnemy* enemy,float dt) {
    enemy->SelectNetworkTarget(init->mpPlayer,Ready()?remotePlayer:nullptr,dt);
    auto* target=enemy->GetNetworkTarget();
    if(!target) target=init->mpPlayer;
    PlayerScope scope(init,target,target==remotePlayer?remoteHands:init->mpPlayerHands);
    enemy->iGameEntity::OnUpdate(dt);
}
bool cCoopNetwork::EnemyTrigger(iGameEnemy* enemy,cGameTrigger* trigger) {
    auto* target=enemy->GetNetworkTarget(); if(!target) target=init->mpPlayer;
    PlayerScope scope(init,target,target==remotePlayer?remoteHands:init->mpPlayerHands);
    return enemy->HandleTrigger(trigger);
}
cPlayer* cCoopNetwork::PlayerForBody(iPhysicsBody* body) const {
    auto* local=hostPlayer;
    if(local && local->GetCharacterBody() && local->GetCharacterBody()->GetBody()==body) return local;
    if(remotePlayer && remotePlayer->GetCharacterBody() && remotePlayer->GetCharacterBody()->GetBody()==body) return remotePlayer;
    // Enemy scopes may temporarily expose player 2 as init->mpPlayer.
    return nullptr;
}
void cCoopNetwork::DamagePlayer(cPlayer* p,float damage) {
    PlayerScope scope(init,p,p==remotePlayer?remoteHands:hostHands);
    p->Damage(damage,ePlayerDamageType_BloodSplash);
}
void cCoopNetwork::PrepareSave() {
    cPlayer* players[]={hostPlayer,remotePlayer};
    for(auto* player:players) {
        if(!player || !player->GetCharacterBody()) continue;
        PlayerScope scope(init,player,player==remotePlayer?remoteHands:hostHands);
        const auto state=player->GetState();
        if(state==ePlayerState_Grab || state==ePlayerState_Move || state==ePlayerState_Push) {
            player->ChangeState(ePlayerState_Normal); player->SetPushBody(nullptr);
        }
    }
}
void cCoopNetwork::SavePartner(cSavedGame* save) {
    save->mlCoopVersion=1;
    save->msHostWeapon=init->mpPlayerHands->GetCurrentModel(1)?init->mpPlayerHands->GetCurrentModel(1)->msName:"";
    PlayerScope scope(init,remotePlayer,remoteHands);
    remotePlayer->SaveToGlobal(&save->mPlayer2);
    save->mPlayer2.mState=ePlayerState_Normal; // A physics grab cannot survive a load.
    save->mPlayer2.mbActive=true;
    save->msClientWeapon=remoteHands->GetCurrentModel(1)?remoteHands->GetCurrentModel(1)->msName:"";
}
void cCoopNetwork::RestoreWeapon(const std::string& model) {
    if(model.empty()) { init->mpPlayer->ChangeState(ePlayerState_Normal); init->mpPlayerHands->SetCurrentModel(1,""); return; }
    for(const auto& pair:init->mpInventory->NetworkItems()) {
        auto* item=pair.second;
        if(item->GetItemType()!=eGameItemType_WeaponMelee || cString::ToLowerCase(item->GetHudModelName())!=cString::ToLowerCase(model)) continue;
        if(!init->mpPlayerHands->GetModel(item->GetHudModelName())) init->mpPlayerHands->AddModelFromFile(item->GetHudModelFile());
        auto* hud=init->mpPlayerHands->GetModel(item->GetHudModelName()); if(!hud) return;
        // Inventory's normal action is a toggle; restoration must be idempotent.
        init->mpPlayerHands->SetCurrentModel(1,item->GetHudModelName());
        auto* state=static_cast<cPlayerState_WeaponMelee*>(init->mpPlayer->GetStateData(ePlayerState_WeaponMelee));
        state->SetHudWeapon(static_cast<cHudModel_WeaponMelee*>(hud));
        init->mpPlayer->ChangeState(ePlayerState_WeaponMelee); return;
    }
}
void cCoopNetwork::LoadPartner(cSavedGame* save) {
    RestoreWeapon(save->msHostWeapon); Log("[COOP/SAVE] Host weapon restored state=%d\n",int(init->mpPlayer->GetState()));
    {
        PlayerScope scope(init,remotePlayer,remoteHands);
        remotePlayer->ChangeState(ePlayerState_Normal); remotePlayer->LoadFromGlobal(&save->mPlayer2); RestoreWeapon(save->msClientWeapon); Log("[COOP/SAVE] Client weapon restored state=%d\n",int(remotePlayer->GetState()));
    }
    Log("[COOP/SAVE] Restored host_health=%.2f client_health=%.2f host_power=%.2f client_power=%.2f host_pos=%s client_pos=%s host_weapon=%s client_weapon=%s\n",init->mpPlayer->GetHealth(),remotePlayer->GetHealth(),init->mpPlayer->GetPower(),remotePlayer->GetPower(),init->mpPlayer->GetCharacterBody()->GetPosition().ToString().c_str(),remotePlayer->GetCharacterBody()->GetPosition().ToString().c_str(),save->msHostWeapon.c_str(),save->msClientWeapon.c_str());
    deathTime[0]=deathTime[1]=0; lifeMask=0; latest.players[0]=Capture(init->mpPlayer,init->mpPlayerHands);
    latest.players[1]=Capture(remotePlayer,remoteHands); lastInput=0;
    baselineSent=false; published.clear(); inventoryBytes.clear(); journalBytes.clear(); mediaBytes.clear();
}
void cCoopNetwork::RequestSave() {
    if(IsClient()) ClientAction(online::PlayerAction::Checkpoint,true);
    else if(IsHost()) checkpointRequested=true;
}
void cCoopNetwork::UpdateLife(float dt) {
    cPlayer* players[2]={init->mpPlayer,remotePlayer};
    for(int n=0;n<2;++n) deathTime[n]=players[n]->IsDead()?deathTime[n]+dt:0;
    if(deathTime[0]>=5 && deathTime[1]>=5) {
        if(init->mpSaveHandler->LoadCheckpoint()) return;
        Log("[COOP/LIFE] No valid checkpoint; restarting current map\n");
        const std::string file=cString::GetFileName(map.file);
        init->mpMapHandler->Load(file,"link1"); init->mpPlayer->SetHealth(100); remotePlayer->SetHealth(100);
        deathTime[0]=deathTime[1]=0; return;
    }
    for(int n=0;n<2;++n) if(deathTime[n]>=5 && !players[1-n]->IsDead()) {
        auto* p=players[n]; auto* body=p->GetCharacterBody();
        const auto buddy=players[1-n]->GetCharacterBody()->GetFeetPosition();
        bool placed=false;
        for(int trial=0;trial<24 && !placed;++trial) {
            const float angle=trial*k2Pif/8, radius=.8f+.4f*(trial/8);
            const auto feet=buddy+cVector3f(std::cos(angle)*radius,.1f,std::sin(angle)*radius);
            auto matrix=cMath::MatrixTranslate(feet+cVector3f(0,body->GetSize().y/2,0));
            if(!init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld()->CheckShapeWorldCollision(nullptr,body->GetShape(),matrix,body->GetBody(),false,true,nullptr,false)) {
                PlayerScope scope(init,p,n?remoteHands:init->mpPlayerHands);
                body->SetForce(cVector3f(0)); body->SetForceVelocity(cVector3f(0)); body->SetMoveSpeed(eCharDir_Forward,0); body->SetMoveSpeed(eCharDir_Right,0); body->SetFeetPosition(feet); p->SetHealth(100); p->SetActive(true); p->ChangeState(ePlayerState_Normal);
                deathTime[n]=0; placed=true; worldDirty=true;
                Log("[COOP/LIFE] Respawn player=%d near surviving partner\n",n+1);
            }
        }
    }
}
void cCoopNetwork::PublishJournal() {
    cNotebook_GlobalSave saved; init->mpNotebook->SaveToGlobal(&saved);
    online::JournalState next; next.epoch=map.epoch;
    auto notes=saved.mlstNotes.GetIterator();
    while(notes.HasNext()) { const auto& n=notes.Next(); online::JournalEntry e; e.kind=0; e.name=cString::To8Char(n.msName); e.group=n.msTextCat; e.textEntry=n.msTextEntry; next.entries.push_back(e); }
    auto tasks=saved.mlstTasks.GetIterator();
    while(tasks.HasNext()) { const auto& t=tasks.Next(); online::JournalEntry e; e.kind=2; e.name=t.msName; e.textEntry=cString::To8Char(t.msText); next.entries.push_back(e); }
    const auto bytes=online::EncodeJournal(next);
    if(bytes!=journalBytes) { next.revision=journalRevision+1; if(session.SendJournal(next)) { journalBytes=bytes; journalRevision=next.revision; } }
}
void cCoopNetwork::WeaponHit(const std::string& entity,float health) {
    if(!IsHost()) return;
    worldDirty=true;
    if(getenv("PENUMBRA_COOP_QA")) {
        if(IsRemoteContext()) ++clientHits; else ++hostHits;
        Log("[COOP/QA] Authoritative hammer hit role=%s entity=%s health=%.1f t_ms=%llu\n",IsRemoteContext()?"client":"host",entity.c_str(),health,(unsigned long long)Milliseconds());
    }
}
void cCoopNetwork::BeforeObjectBreak(iGameEntity* entity) {
    if(!IsHost() || !remotePlayer || !entity) return;
    const auto state=remotePlayer->GetState();
    if(state!=ePlayerState_Grab && state!=ePlayerState_Move && state!=ePlayerState_Push) return;
    for(int n=0;n<entity->GetBodyNum();++n) if(remotePlayer->GetPushBody()==entity->GetBody(n)) {
        PlayerScope scope(init,remotePlayer,remoteHands);
        if(getenv("PENUMBRA_COOP_QA")) Log("[COOP/QA] Release partner holding broken entity=%s state=%d\n",entity->GetName().c_str(),int(state));
        remotePlayer->ChangeState(ePlayerState_Normal); remotePlayer->SetPushBody(nullptr);
        break;
    }
}
void cCoopNetwork::RelayParticle(const std::string& file,const cMatrixf& matrix) {
    if(!IsHost() || !session.Connected() || file.empty()) return;
    online::MediaState s; s.kind=online::MediaKind::Particle;
    s.name="CoopBreak_"+std::to_string(++sequence); s.id=EntityID(s.name); s.file=file; s.transform=Pack(matrix);
    s.flags=1; s.values[8]=s.values[9]=s.values[10]=1000;
    if(pendingMedia.size()<256) pendingMedia.push_back(s);
    worldDirty=true;
}
bool cCoopNetwork::ClientLook(float yaw,float pitch) {
    if(!IsClient()) return false;
    lookYaw+=yaw; lookPitch+=pitch;
    if(interactHeld && init->mpPlayer->GetState()==ePlayerState_WeaponMelee) {
        auto direction=[](float v) { return v>.015f?1:v<-.015f?-1:0; };
        const int x=direction(yaw),y=direction(pitch);
        if((x && x!=gestureYaw) || (y && y!=gesturePitch)) {
            online::ActionRequest a; a.epoch=map.epoch; a.sequence=++actionSequence; a.action=online::PlayerAction::WeaponLook;
            a.lookYaw=int16_t(std::clamp(yaw*10000,-31420.0f,31420.0f)); a.lookPitch=int16_t(std::clamp(pitch*10000,-31420.0f,31420.0f));
            if(Ready()) session.SendAction(a);
            if(x) gestureYaw=x; if(y) gesturePitch=y;
        }
    }
    return interactHeld && (latest.players[1].flags&16);
}
bool cCoopNetwork::RelayMessage(const tWString& text) {
    if(!IsHost()) return false;
    online::Presentation e; e.kind=online::PresentationKind::Message; e.target=IsRemoteContext()?2:0; e.text[0]=cString::To8Char(text); pendingPresentation.push_back(e);
    return IsRemoteContext();
}
void cCoopNetwork::RelayFade(bool out,float seconds) {
    if(!IsHost()) return;
    online::Presentation e; e.kind=online::PresentationKind::Fade; e.values[0]=out?1:0; e.values[1]=int32_t(std::max(0.0f,seconds)*1000); pendingPresentation.push_back(e);
}
void cCoopNetwork::RelayGuiSound(const std::string& file,float volume) {
    if(!IsHost()) return;
    online::Presentation e; e.kind=online::PresentationKind::GuiSound; e.target=IsRemoteContext()?2:0; e.text[0]=file; e.values[0]=int32_t(std::clamp(volume,0.0f,1.0f)*1000); pendingPresentation.push_back(e);
}
void cCoopNetwork::Update(float dt) {
    if(!IsOnline()) return;
    steam.Update();
    session.Poll();
    RunQA(dt);
    if(session.Connected() && generation!=session.Generation()) {
        generation=session.Generation(); baselineSent=false; positioned=false; restoredEquipment=false; published.clear(); motion.clear(); prediction.Clear();
        inventoryBytes.clear(); journalBytes.clear(); mediaBytes.clear();
        if(IsHost() && map.epoch) { ++map.epoch; session.SendMap(map); }
        hadConnection=true;
    }
    if(!session.Connected()) {
        if(IsClient()) init->mpPlayer->SetActive(false);
        if(hadConnection && IsClient() && !session.AttemptActive() && session.RetryAllowed()) session.StartClient(session.RemoteAddress(),session.RemotePort(),true);
        if(remotePlayer && remotePlayer->GetCharacterBody()) { PlayerScope scope(init,remotePlayer,remoteHands); remotePlayer->SetActive(false); remotePlayer->GetCharacterBody()->SetMoveSpeed(eCharDir_Forward,0); remotePlayer->GetCharacterBody()->SetMoveSpeed(eCharDir_Right,0); }
        return;
    }
    const auto perfStart=std::chrono::steady_clock::now();
    (IsHost()?hostDiag.clock:clientDiag.clock).Tick(Milliseconds());
    InjectQAHitch();
    if(IsHost()) UpdateHost(dt); else UpdateClient(dt);
    ReportDiagnostics();
    const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-perfStart).count());
    perfSum+=elapsed; perfMax=std::max(perfMax,elapsed); ++perfSamples; perfTime+=dt;
    if(perfTime>=5) {
        Log("[COOP/PERF] role=%s average_us=%llu max_us=%llu samples=%u\n",IsHost()?"host":"client",(unsigned long long)(perfSum/perfSamples),(unsigned long long)perfMax,perfSamples);
        perfTime=0; perfSum=perfMax=0; perfSamples=0;
    }
}
void cCoopNetwork::UpdateHost(float dt) {
    if(!map.epoch || !remotePlayer->GetCharacterBody()) return;
    const bool canPlay=Ready() && init->mpGame->GetUpdater()->GetCurrentContainerName()=="Default";
    // The physics step that opened this tick integrated the previous command,
    // so the partner's pose is captured now and acknowledges exactly that one.
    // Applying this tick's command first would send a pose that lacks it.
    const online::PlayerState remotePose=Capture(remotePlayer,remoteHands); const uint32_t remoteAck=lastInput;
    // One queued command per tick (protocol 4); `repeated` replays the previous
    // command while the jitter buffer refills.
    online::PlayerInput input; bool repeated=false;
    const bool hasInput=canPlay && !remotePlayer->IsDead() && session.NextInput(input,repeated) && input.epoch==map.epoch && !(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))!="hammer" && ((qaTime>4 && qaTime<6) || (qaTime>15 && qaTime<18) || (qaTime>24 && qaTime<26)));
    if(canPlay && !remotePlayer->IsDead()) {
        // Diagnostics: how client commands map onto host ticks. The host keeps
        // only the newest input, so commands that arrive together are skipped.
        hostDiag.queueDepth.Add(double(session.InputQueueDepth())); hostDiag.queueTarget=unsigned(session.InputQueueTarget());
        if(!hasInput) ++hostDiag.idleTicks;
        else if(!repeated) {
            const uint32_t advance=input.sequence-lastInput;
            if(lastInput && advance<1000) { hostDiag.skippedInputs+=advance-1; hostDiag.commandDrift+=int64_t(advance)-1; }
            const uint64_t now=Milliseconds();
            if(hostDiag.lastArrival) hostDiag.inputInterval.Add(double(now-hostDiag.lastArrival));
            hostDiag.lastArrival=now; ++hostDiag.newInputs;
        } else { ++hostDiag.reusedTicks; --hostDiag.commandDrift; }
    }
    {
        PlayerScope scope(init,remotePlayer,remoteHands);
        remotePlayer->SetActive((init->mpPlayer==remotePlayer ? (hostPlayer->IsActive() || hostPlayer->IsDead()) : true) && !remotePlayer->IsDead());
        // Local UI and camera effects belong to their owner; remote physics has its own state.
        if(hasInput) {
            if(remotePlayer->GetState()==ePlayerState_Move || remotePlayer->GetState()==ePlayerState_WeaponMelee) {
                if(!repeated && remotePlayer->GetState()==ePlayerState_Move) { remotePlayer->AddYaw(input.lookYaw/10000.0f); remotePlayer->AddPitch(input.lookPitch/10000.0f); }
                if(remotePlayer->GetState()==ePlayerState_WeaponMelee && remoteSimpleSwing) remotePlayer->AddYaw(0);
                if(remotePlayer->GetState()==ePlayerState_WeaponMelee && !qaAimLocked) { remotePlayer->GetCamera()->SetYaw(input.yaw/1000.0f); remotePlayer->GetCamera()->SetPitch(input.pitch/1000.0f); remotePlayer->GetCharacterBody()->SetYaw(input.yaw/1000.0f); }
            } else if(!qaAimLocked && !(getenv("PENUMBRA_COOP_QA") && qaStage==2 && qaTime<8.6f && remotePlayer->GetState()==ePlayerState_Normal)) {
                remotePlayer->GetCamera()->SetYaw(input.yaw/1000.0f); remotePlayer->GetCamera()->SetPitch(input.pitch/1000.0f); remotePlayer->GetCharacterBody()->SetYaw(input.yaw/1000.0f);
            }
            if((input.buttons&4) && !(remoteButtons&4)) remotePlayer->StartRun();
            if(!(input.buttons&4) && (remoteButtons&4)) remotePlayer->StopRun();
            if((input.buttons&2) && !(remoteButtons&2)) remotePlayer->StartCrouch();
            if(!(input.buttons&2) && (remoteButtons&2)) remotePlayer->StopCrouch();
            if((input.buttons&1) && !(remoteButtons&1)) remotePlayer->Jump();
            remotePlayer->SetJumpButtonDown((input.buttons&1)!=0); remoteButtons=input.buttons; lastInput=input.sequence;
            remotePlayer->MoveForwards(input.forward/1000.0f,dt); remotePlayer->MoveSideways(input.strafe/1000.0f,dt);
        } else { remotePlayer->StopInteract(); remotePlayer->MoveForwards(0,dt); remotePlayer->MoveSideways(0,dt); remoteButtons=0; remoteExamineDown=false; }
        remotePlayer->Update(dt);
        if(!remotePlayer->IsDead()) remotePlayer->AddHealth(dt*(init->mDifficulty==eGameDifficulty_Easy?1.4f:init->mDifficulty==eGameDifficulty_Normal?.4f:.14f));
        remotePlayer->GetFlashLight()->Update(dt);
        const float hp=remotePlayer->GetHealth();
        remotePlayer->SetHealthSpeedMul(hp>50?1.0f:hp>25?.8f:hp>10?.6f:.4f);
        remoteHands->Update(dt);
        // The partner's world tool is drawn separately; these private HUD entities drive combat.
        for(int slot=0;slot<2;++slot) if(auto* model=remoteHands->GetCurrentModel(slot)) {
            if(model->GetEntity()) model->GetEntity()->SetVisible(false);
            for(auto* light:model->mvLights) light->SetVisible(false);
            for(auto* bill:model->mvBillboards) bill->SetVisible(false);
        }
    }
    online::ActionRequest action;
    while(session.ReadAction(action)) {
        if(action.epoch!=map.epoch || !canPlay || remotePlayer->IsDead()) continue;
        if(action.action==online::PlayerAction::Checkpoint) { checkpointRequested=true; continue; }
        if(action.action==online::PlayerAction::Interact && action.pressed && init->mpGameMessageHandler->HasMessage()) { init->mpGameMessageHandler->ShowNext(); continue; }
        PlayerScope scope(init,remotePlayer,remoteHands);
        if(action.action==online::PlayerAction::Interact) {
            if(action.pressed && !action.item.empty()) {
                auto* item=init->mpInventory->GetItem(action.item);
                auto* body=remotePlayer->GetPickedBody();
                auto* entity=body?static_cast<iGameEntity*>(body->GetUserData()):nullptr;
                if(getenv("PENUMBRA_COOP_QA")) Log("[COOP/QA] Item action name=%s target=%s distance=%.3f maximum=%.3f\n",action.item.c_str(),entity?entity->GetName().c_str():"none",remotePlayer->GetPickedDist(),entity?entity->GetMaxInteractDist():0);
                if(item && entity && entity->IsActive() && remotePlayer->GetPickedDist()<=entity->GetMaxInteractDist())
                    init->mpInventory->GetItemType(item->GetItemType())->OnUse(item,entity);
            } else if(action.pressed) {
                if(getenv("PENUMBRA_COOP_QA")) {
                    auto eye=remotePlayer->GetCamera()->GetPosition(), forward=remotePlayer->GetCamera()->GetForward();
                    Log("[COOP/QA] Native interact picked=%s distance=%.3f pitch=%.3f eye=%.3f/%.3f/%.3f forward=%.3f/%.3f/%.3f\n",remotePlayer->GetPickedBody()?remotePlayer->GetPickedBody()->GetName().c_str():"none",remotePlayer->GetPickedDist(),remotePlayer->GetCamera()->GetPitch(),eye.x,eye.y,eye.z,forward.x,forward.y,forward.z);
                }
                remotePlayer->StartInteract();
            } else remotePlayer->StopInteract();
        }
        if(action.action==online::PlayerAction::Attack) { remoteExamineDown=action.pressed; if(action.pressed) remotePlayer->StartExamine(); else remotePlayer->StopExamine(); }
        if(action.action==online::PlayerAction::Lantern && (!action.pressed || init->mpInventory->GetItem("flashlight"))) remotePlayer->GetFlashLight()->SetActive(action.pressed);
        if(action.action==online::PlayerAction::UseInventory) {
            if(!action.pressed) remotePlayer->StartHolster();
            else if(auto* item=init->mpInventory->GetItem(action.item)) {
                if(item->GetItemType()==eGameItemType_WeaponMelee) {
                    remoteSimpleSwing=action.secondItem=="simple";
                    auto* equipped=remoteHands->GetCurrentModel(1);
                    RestoreWeapon(item->GetHudModelName());
                }
            }
        }
        if(action.action==online::PlayerAction::WeaponLook && remotePlayer->GetState()==ePlayerState_WeaponMelee) {
            if(auto* model=remoteHands->GetCurrentModel(1))
                static_cast<cHudModel_WeaponMelee*>(model)->OnMouseMove(cVector2f(action.lookYaw/10000.0f,action.lookPitch/10000.0f));
        }
    }
    if(session.IsMapReady(map.epoch) && !baselineSent) {
        if(PublishWorld(true) && session.SendWorldReady(map.epoch)) baselineSent=true;
    }
    if(session.IsMapReady(map.epoch)) {
        PublishMedia();
        journalTime+=dt; if(journalTime>=.5f || journalBytes.empty()) { journalTime=0; PublishJournal(); }
        online::InventoryState next; next.epoch=map.epoch;
        for(const auto& pair:init->mpInventory->NetworkItems()) {
            auto* item=pair.second; online::InventoryItem value; value.name=item->GetName(); value.image=item->GetEntityFile(); value.count=uint16_t(std::max(1,item->GetCount())); next.items.push_back(value);
        }
        auto bytes=online::EncodeInventory(next);
        if(bytes!=inventoryBytes) {
            next.revision=inventory.revision+1;
            if(session.SendInventory(next)) { inventory=next; inventoryBytes=bytes; }
        }
        for(auto it=pendingPresentation.begin();it!=pendingPresentation.end();) { it->epoch=map.epoch; if(session.SendPresentation(*it)) it=pendingPresentation.erase(it); else break; }
        worldTime+=dt; if(worldDirty || worldTime+0.000001f>=1.0f/30) { worldTime=std::max(0.0f,worldTime-1.0f/30); worldDirty=false; PublishWorld(false); }
        sendTime+=dt; backupTime+=dt;
        if(sendTime+0.000001f>=1.0f/60) {
            sendTime=std::max(0.0f,sendTime-1.0f/60); latest.epoch=map.epoch; latest.sequence=++sequence; latest.acknowledgedInput=remoteAck;
            latest.players[0]=Capture(init->mpPlayer,init->mpPlayerHands); latest.players[1]=remotePose;
            latest.players[1].flags=(latest.players[1].flags&~4)|((canPlay && !remotePlayer->IsDead() && ((latest.players[0].flags&4) || (latest.players[0].flags&8)))?4:0);
            const uint8_t deadMask=(init->mpPlayer->IsDead()?1:0)|(remotePlayer->IsDead()?2:0);
            if(deadMask!=lifeMask) { session.SendLife(latest); lifeMask=deadMask; }
            session.SendSnapshot(latest);
            if(backupTime>=0.2f) { session.SendSnapshotBackup(latest); backupTime=0; }
        }
        UpdateAvatar(latest.players[1],dt);
    }
    checkpointCooldown=std::max(0.0f,checkpointCooldown-dt);
    if(checkpointRequested && checkpointCooldown<=0 && Ready()) {
        checkpointRequested=false; checkpointCooldown=2;
        if(init->mpSaveHandler->SaveCheckpoint()) { online::Presentation e; e.epoch=map.epoch; e.kind=online::PresentationKind::Hint; e.text[0]="Checkpoint co-op salvato sull\x27host."; session.SendPresentation(e); init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(e.text[0]),3,false); }
    }
    if(Ready()) UpdateLife(dt);
}
void cCoopNetwork::UpdateClient(float dt) {
    online::MapState received;
    if(session.ReadMap(received) && received.epoch!=map.epoch) {
        if(Fingerprint(received.file)!=received.fingerprint) { Log("[COOP/MAP] Data mismatch %s\n",received.file.c_str()); session.Stop(); return; }
        map=received;
        if(!init->mpMapHandler->Load(cString::GetFileName(map.file),"link1")) { session.Stop(); return; }
    }
    if(!map.epoch || !init->mpPlayer->GetCharacterBody()) return;
    online::EntityState entity; while(session.ReadEntity(entity)) if(entity.epoch==map.epoch) { if(!session.IsWorldApplied(map.epoch) && !(entity.flags&4)) baselineNames.insert(entity.name); ApplyEntity(entity); }
    online::MediaState media; while(session.ReadMedia(media)) if(media.epoch==map.epoch) ApplyMedia(media);
    online::Snapshot snapshot,life;
    bool hasSnapshot=session.ReadSnapshot(snapshot);
    while(session.ReadLife(life)) if(life.epoch==map.epoch && (!hasSnapshot || int32_t(life.sequence-snapshot.sequence)>0)) { snapshot=life; hasSnapshot=true; }
    if(hasSnapshot && snapshot.epoch==map.epoch) {
        latest=snapshot;
        if(snapshot.sequence!=lastSnapshot) {
            const uint64_t now=Milliseconds();
            if(lastSnapshotAt && std::string(getenv("PENUMBRA_COOP_QA")?getenv("PENUMBRA_COOP_QA"):"")=="hammer" && hammerTime>40 && hammerTime<48) { const float interval=(now-lastSnapshotAt)/1000.0f; intervalSum+=interval; intervalMax=std::max(intervalMax,interval); ++intervalCount; }
            lastSnapshotAt=now;
            if(clientDiag.lastArrival) clientDiag.snapshotInterval.Add(double(now-clientDiag.lastArrival));
            clientDiag.lastArrival=now; ++clientDiag.snapshots;
            const double hostStep=clientDiag.HostPose(snapshot.players[1].x/1000.0f,snapshot.players[1].z/1000.0f);
            { const auto& h=snapshot.players[1]; const float f=h.forwardSpeed/1000.0f, r=h.sideSpeed/1000.0f; const float cap=(f>=0?h.forwardMax:h.backwardMax)/1000.0f;
              if(f||r) { const double nominal=std::min(std::sqrt(f*f+r*r),cap)/60; clientDiag.nominalStep.Add(nominal*100);
                  if(hostStep>=0 && hostStep<1) { ++clientDiag.movingPoses; if(hostStep<nominal*.2) ++clientDiag.stalledPoses; clientDiag.lastPoseFree=hostStep>=nominal*.8; } }
              else clientDiag.lastPoseFree=false; }
            snapshotAge=0;
            auto* player=init->mpPlayer; auto* body=player->GetCharacterBody(); const auto& s=snapshot.players[1];
            cVector3f target(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f);
            if(!positioned) {
                body->SetFeetPosition(target); body->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetPitch(s.pitch/1000.0f);
                positioned=true; Log("[COOP/STATE] First player baseline applied\n");
            } else if((target-body->GetFeetPosition()).SqrLength()>4.0f) {
                if(hammerTime>40 && hammerTime<48) ++hardCorrections;
                ++clientDiag.hard2m;
                body->SetFeetPosition(target); prediction.Clear();
            }
            player->SetActive((s.flags&4)!=0); player->SetHealth(s.health/100.0f); player->SetPower(s.oil/100.0f);
            if(s.flags&16) { player->GetCamera()->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetPitch(s.pitch/1000.0f); }
            lastSnapshot=snapshot.sequence;
        }
        if(positioned && (snapshot.players[1].flags&4) && !(snapshot.players[1].flags&8)) {
            auto* body=init->mpPlayer->GetCharacterBody(); auto* physics=init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld();
            online::PredictionCollisionCache cache;
            const float height=body->GetPosition().y, step=body->GetMaxStepSize();
            const bool grounded=body->IsOnGround();
            auto collide=[&](online::PredictedPosition from,online::PredictedPosition to) {
                const bool blocked=cache.Blocked(to.x,to.z,[&](float x,float z) {
                    // Resting floor and slope contacts must not freeze the replay at
                    // the delayed host pose (rubber banding on outdoor terrain).
                    // Same query parameters as before; only the push is now inspected.
                    return online::StepBlocked(height,step,grounded,[&](float y) {
                        cMatrixf matrix=cMatrixf::Identity; matrix.SetTranslation(cVector3f(x,y,z));
                        cVector3f pushed;
                        if(!physics->CheckShapeWorldCollision(&pushed,body->GetShape(),matrix,body->GetBody(),false,true,nullptr,false)) return online::PredictionContact{};
                        return online::PredictionContact{pushed.x-x,pushed.y-y,pushed.z-z};
                    });
                });
                ++clientDiag.replaySteps; if(blocked) ++clientDiag.replayBlocked;
                return blocked?from:to;
            };
            // Replay with this body's own limits: run, crouch and health speed
            // are local move states the host applies to the same commands.
            online::MovementLimits limits;
            limits.forwardMax=body->GetMaxPositiveMoveSpeed(eCharDir_Forward); limits.backwardMax=-body->GetMaxNegativeMoveSpeed(eCharDir_Forward);
            limits.sideMax=body->GetMaxPositiveMoveSpeed(eCharDir_Right);
            limits.forwardAcc=body->GetMoveAcc(eCharDir_Forward); limits.forwardDec=body->GetMoveDeacc(eCharDir_Forward);
            limits.sideAcc=body->GetMoveAcc(eCharDir_Right); limits.sideDec=body->GetMoveDeacc(eCharDir_Right);
            online::PredictedPosition pos;
            if(KinematicPrediction()) pos=prediction.ReconcileSmoothed(snapshot.players[1],snapshot.acknowledgedInput,snapshot.sequence,dt,limits,collide);
            else { NativeStepper stepper(body); pos=prediction.ReconcileSmoothedWith(snapshot.players[1],snapshot.acknowledgedInput,snapshot.sequence,dt,stepper,collide); }
            float error=0; bool reset=false;
            if(prediction.TakePoseCorrection(error,reset)) {
                clientDiag.correction.Add(error); if(clientDiag.lastPoseFree) clientDiag.freeCorrection.Add(error); if(error>.02f) ++clientDiag.over2cm; if(error>.1f) ++clientDiag.over10cm; if(reset) ++clientDiag.reset25cm;
            }
            if(prediction.LastReplayTruncated()) ++clientDiag.replayTruncated;
            clientDiag.pending.Add(double(prediction.Pending()));
            clientDiag.ackLag.Add(double(uint32_t(inputSequence-snapshot.acknowledgedInput)));
            if(hammerTime>40 && hammerTime<48) correctionMax=std::max(correctionMax,prediction.CorrectionDistance());
            body->SetNetworkHorizontalPosition(pos.x,pos.z);
        }
        if(positioned && !online::MovementPrediction::Predicts(snapshot.players[1])) ++clientDiag.unpredictedTicks;
        UpdateAvatar(snapshot.players[0],dt);
    }
    if(positioned) { const auto feet=init->mpPlayer->GetCharacterBody()->GetFeetPosition(); clientDiag.ShownPose(feet.x,feet.z); }
    snapshotAge+=dt;
    for(auto& pair:motion) {
        auto* e=init->mpMapHandler->GetGameEntity(pair.first,false); if(!e) continue;
        auto& m=pair.second; m.age+=dt; const float alpha=std::min(1.0f,m.age/(1.0f/30));
        if(m.meshMotion && e->GetMeshEntity()) e->GetMeshEntity()->SetMatrix(cMath::MatrixSlerp(alpha,m.meshFrom,Unpack(m.state.transform),true));
        for(size_t n=0;n<m.state.bodies.size() && n<size_t(e->GetBodyNum());++n) e->GetBody(int(n))->SetMatrix(cMath::MatrixSlerp(alpha,m.from[n],Unpack(m.state.bodies[n]),true));
    }
    online::Presentation event;
    while(session.ReadPresentation(event)) {
        if(event.epoch!=map.epoch || event.target==1) continue;
        if(event.kind==online::PresentationKind::Hint) init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(event.text[0]),3,false);
        if(event.kind==online::PresentationKind::Message) init->mpGameMessageHandler->Add(cString::To16Char(event.text[0]));
        if(event.kind==online::PresentationKind::Fade) { if(event.values[0]) init->mpFadeHandler->FadeOut(event.values[1]/1000.0f); else init->mpFadeHandler->FadeIn(event.values[1]/1000.0f); }
        if(event.kind==online::PresentationKind::GuiSound) init->mpGame->GetSound()->GetSoundHandler()->PlayGui(event.text[0],false,event.values[0]/1000.0f);
        auto* renderer=init->mpGame->GetGraphics()->GetRenderer3D();
        const cColor color(event.values[0]/1000.0f,event.values[1]/1000.0f,event.values[2]/1000.0f,event.values[3]/1000.0f);
        if(event.kind==online::PresentationKind::FogColor) renderer->SetFogColor(color);
        if(event.kind==online::PresentationKind::FogProperties) {
            renderer->SetFogStart(event.values[0]/1000.0f); renderer->SetFogEnd(event.values[1]/1000.0f);
            renderer->SetFogActive((event.values[3]&1)!=0); renderer->SetFogCulling((event.values[3]&2)!=0);
        }
        if(event.kind==online::PresentationKind::Light && event.text[0]=="coop_ambient") renderer->SetAmbientColor(color);
        if(event.kind==online::PresentationKind::Light && event.text[0]=="coop_skybox") {
            const auto& file=event.text[1];
            if(file.find("..")!=std::string::npos || file.find(':')!=std::string::npos || (!file.empty() && (file[0]=='/' || file[0]=='\\'))) continue;
            if(file!=currentSkybox) { renderer->SetSkyBox(file.empty()?nullptr:init->mpGame->GetResources()->GetTextureManager()->CreateCubeMap(file,false),!file.empty()); currentSkybox=file; }
            renderer->SetSkyBoxColor(color); renderer->SetSkyBoxActive(event.values[4]!=0);
        }
    }
    online::InventoryState items;
    if(session.ReadInventory(items) && items.epoch==map.epoch && (items.revision!=inventory.revision || items.epoch!=inventory.epoch)) {
        applyingBaseline=true;
        std::vector<cInventoryItem*> remove;
        for(const auto& pair:init->mpInventory->NetworkItems()) {
            bool present=false; for(const auto& value:items.items) if(value.name==pair.first) present=true;
            if(!present) remove.push_back(pair.second);
        }
        for(auto* item:remove) {
            if(init->mpInventory->IsActive()) init->mpInventory->SetActive(false);
            if(init->mpPlayer->GetCurrentItem()==item) { init->mpPlayer->ChangeState(ePlayerState_Normal); init->mpPlayer->SetCurrentItem(nullptr); }
            init->mpInventory->RemoveItem(item);
        }
        for(const auto& value:items.items) {
            if(value.image.empty() || value.image.find("..")!=std::string::npos || value.image.find(':')!=std::string::npos || cString::GetFileExt(value.image)!="ent") continue;
            auto* item=init->mpInventory->GetItem(value.name);
            if(!item) { init->mpInventory->AddItemFromFile(value.name,value.image,-1); item=init->mpInventory->GetItem(value.name); }
            if(item) item->SetCount(value.count);
        }
        inventory=items; applyingBaseline=false;
    }
    // Confirm only after every baseline presentation queued in this poll was applied.
    online::JournalState book;
    if(session.ReadJournal(book) && book.epoch==map.epoch && book.revision!=receivedJournal) {
        cNotebook_GlobalSave saved;
        for(const auto& e:book.entries) {
            if(e.kind==0) { cNotebookNote_GlobalSave n; n.msName=cString::To16Char(e.name); n.msTextCat=e.group; n.msTextEntry=e.textEntry; n.mbRead=false; saved.mlstNotes.Add(n); }
            if(e.kind==2) { cNotebookTask_GlobalSave t; t.msName=e.name; t.msText=cString::To16Char(e.textEntry); saved.mlstTasks.Add(t); }
        }
        if(init->mpNotebook->IsActive()) init->mpNotebook->SetActive(false);
        init->mpNotebook->Reset(); init->mpNotebook->LoadFromGlobal(&saved); receivedJournal=book.revision;
    }
    if(positioned && inventory.epoch==map.epoch && receivedJournal && !session.IsWorldApplied(map.epoch) && session.IsWorldReady(map.epoch)) {
        auto it=init->mpMapHandler->GetGameEntityIterator();
        while(it.HasNext()) { auto* e=it.Next(); if(!baselineNames.count(e->GetName())) e->SetActive(false); }
        applyingBaseline=true;
        const auto& s=latest.players[1];
        init->mpPlayer->GetFlashLight()->SetActive((s.flags&2)!=0);
        for(const auto& pair:init->mpInventory->NetworkItems()) if(pair.second->GetItemType()==eGameItemType_WeaponMelee) {
            const auto model=cString::ToLowerCase(pair.second->GetHudModelName());
            if((s.tinderboxes==1 && model=="hammer") || (s.tinderboxes==2 && model=="pickaxe")) RestoreWeapon(model);
        }
        applyingBaseline=false; restoredEquipment=true;
        session.SendWorldApplied(map.epoch);
    }
    if(Ready()) {
        online::PlayerInput input; input.epoch=map.epoch; input.sequence=++inputSequence;
        auto* p=init->mpPlayer; auto* keys=init->mpGame->GetInput();
        if(init->mpGame->GetUpdater()->GetCurrentContainerName()=="Default" && p->IsActive() && !init->mpGameMessageHandler->HasMessage()) {
            input.forward=(keys->IsTriggerd("Forward")?1000:0)-(keys->IsTriggerd("Backward")?1000:0);
            input.strafe=(keys->IsTriggerd("Right")?1000:0)-(keys->IsTriggerd("Left")?1000:0);
            input.buttons=(keys->IsTriggerd("Jump")?1:0)|(p->GetMoveState()==ePlayerMoveState_Crouch?2:0)|(keys->IsTriggerd("Run")?4:0);
        }
        if(getenv("PENUMBRA_COOP_QA")) { input.forward=input.strafe=0; input.buttons=0; }
        if(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))!="hammer" && qaTime>2 && qaTime<3) {
            input.forward=-1000; input.strafe=0; input.buttons=0; p->MoveForwards(-1,dt);
        }
        if(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))=="hammer" && hammerTime>40 && hammerTime<48) {
            const int walk=getenv("PENUMBRA_COOP_QA_WALK")?std::clamp(atoi(getenv("PENUMBRA_COOP_QA_WALK")),100,1000):500; input.forward=int16_t((hammerTime<44)?walk:-walk); p->MoveForwards(input.forward/1000.0f,dt);
            if(hasClientPosition) { auto delta=p->GetCharacterBody()->GetFeetPosition()-lastClientPosition; delta.y=0; const float step=delta.Length(); stepMax=std::max(stepMax,step); movementDistance+=step; }
            lastClientPosition=p->GetCharacterBody()->GetFeetPosition(); hasClientPosition=true;
        }
        input.yaw=int16_t(Wrapped(p->GetCamera()->GetYaw())*1000); input.pitch=int16_t(p->GetCamera()->GetPitch()*1000);
        input.lookYaw=int16_t(std::clamp(lookYaw*10000,-31420.0f,31420.0f)); input.lookPitch=int16_t(std::clamp(lookPitch*10000,-31420.0f,31420.0f)); lookYaw=lookPitch=0;
        session.SendInput(input); prediction.Push(input,dt);
    }
}
void cCoopNetwork::ReportDiagnostics() {
    const uint64_t now=Milliseconds();
    if(!diagReportAt) { diagReportAt=now; return; }
    if(now-diagReportAt<5000) return;
    const unsigned window=unsigned(now-diagReportAt); diagReportAt=now;
    char text[512];
    std::snprintf(text,sizeof text,"[COOP/NETDIAG] role=%s window_ms=%u rtt_ms=%u rtt_var_ms=%u",IsHost()?"host":"client",window,session.Ping(),session.PingVariance());
    std::string line=text;
    if(IsHost()) {
        auto& d=hostDiag; d.totalCommandDrift+=d.commandDrift;
        const auto queue=session.TakeInputStats();
        std::snprintf(text,sizeof text," ticks=%u burst_ticks=%u new_input_ticks=%u reused_input_ticks=%u expired_input_ticks=%u skipped_inputs=%llu command_drift=%lld command_drift_total=%lld queue_target=%u queue_starvations=%u queue_dropped=%u queue_lost=%u",
            d.clock.ticks,d.clock.burst,d.newInputs,d.reusedTicks,d.idleTicks,(unsigned long long)d.skippedInputs,(long long)d.commandDrift,(long long)d.totalCommandDrift,d.queueTarget,queue.starvations,queue.dropped,queue.lost);
        line+=text+d.clock.interval.Format("tick_gap_ms")+d.inputInterval.Format("new_input_gap_ms")+d.queueDepth.Format("queue_depth");
        d.ResetWindow();
    } else {
        auto& d=clientDiag;
        std::snprintf(text,sizeof text," ticks=%u burst_ticks=%u snapshots=%u unpredicted_ticks=%u replay_truncated=%u poses_over_2cm=%u poses_over_10cm=%u resets_over_25cm=%u hard_over_2m=%u",
            d.clock.ticks,d.clock.burst,d.snapshots,d.unpredictedTicks,d.replayTruncated,d.over2cm,d.over10cm,d.reset25cm,d.hard2m);
        std::snprintf(text+strlen(text),sizeof text-strlen(text)," host_path_m=%.3f shown_path_m=%.3f replay_steps=%u replay_blocked=%u moving_poses=%u stalled_poses=%u",d.hostPath,d.shownPath,d.replaySteps,d.replayBlocked,d.movingPoses,d.stalledPoses);
        line+=text+d.hostStep.Format("host_step_cm",100)+d.nominalStep.Format("nominal_step_cm")+d.freeCorrection.Format("free_correction_cm",100)+d.clock.interval.Format("tick_gap_ms")+d.snapshotInterval.Format("snapshot_gap_ms")+d.correction.Format("correction_cm",100)+d.pending.Format("pending")+d.ackLag.Format("ack_lag");
        d.ResetWindow();
    }
    Log("%s\n",line.c_str());
}
// QA only: PENUMBRA_COOP_QA_HITCH=host|client stalls that role for 100 ms once per
// second during the scripted hammer walk, like a slow frame on a real PC. HPL1
// then runs the missed 60 Hz logic ticks back to back.
void cCoopNetwork::InjectQAHitch() {
    // QA only: where the partner walks during the hammer walk, to tell blocked
    // poses (crate debris, walls) from netcode errors.
    if(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))=="hammer" && IsHost() && hammerTime>39.5f && hammerTime<48 && remotePlayer && remotePlayer->GetCharacterBody()) {
        static float logClock=0; logClock+=1.0f/60;
        if(logClock>=.25f) {
            logClock=0; auto* body=remotePlayer->GetCharacterBody(); const auto a=body->GetFeetPosition(), b=init->mpPlayer->GetCharacterBody()->GetFeetPosition();
            Log("[COOP/QA] Walk remote=%.2f/%.2f/%.2f host=%.2f/%.2f/%.2f distance_xz=%.2f speed=%.2f/%.2f yaw=%.2f\n",a.x,a.y,a.z,b.x,b.y,b.z,std::sqrt((a.x-b.x)*(a.x-b.x)+(a.z-b.z)*(a.z-b.z)),
                body->GetMoveSpeed(eCharDir_Forward),body->GetMoveSpeed(eCharDir_Right),body->GetYaw());
        }
    }
    const char* role=getenv("PENUMBRA_COOP_QA_HITCH");
    if(!role || !getenv("PENUMBRA_COOP_QA") || std::string(getenv("PENUMBRA_COOP_QA"))!="hammer" || std::string(role)!=(IsHost()?"host":"client")) return;
    if(hammerTime<=40 || hammerTime>=48) { hitchClock=0; return; }
    hitchClock+=1.0f/60;
    if(hitchClock>=1) { hitchClock=0; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
}
bool cCoopNetwork::PublishWorld(bool baseline) {
    bool complete=true;
    std::set<std::string> alive;
    auto it=init->mpMapHandler->GetGameEntityIterator();
    while(it.HasNext()) {
        auto* entity=it.Next(); const auto& name=entity->GetName(); alive.insert(name);
        online::EntityState e; e.epoch=map.epoch; e.id=EntityID(name); e.name=name; e.file=entity->GetFileName();
        e.flags=entity->IsActive()?1:0; e.subtype=uint16_t(entity->GetType());
        if(entity->GetType()==eGameEntityType_Enemy) {
            auto* enemy=static_cast<iGameEnemy*>(entity);
            e.health=int32_t(std::clamp(enemy->GetHealth(),0.0f,100000000.0f)*10);
            e.enemyState=uint8_t(std::max(0,enemy->GetCurrentStateId()));
            e.target=enemy->GetNetworkTarget()==remotePlayer?2:1;
            auto* body=enemy->GetMover()->GetCharBody();
            auto frame=cMath::MatrixRotateY(body->GetYaw()); frame.SetTranslation(body->GetPosition()); e.character=Pack(frame);
            if(auto* mesh=enemy->GetMeshEntity()) for(int n=0;n<mesh->GetAnimationStateNum() && n<1024;++n) {
                auto* a=mesh->GetAnimationState(n); if(!a->IsActive()) continue;
                online::Animation value; value.index=uint16_t(n); value.weight=uint16_t(std::clamp(a->GetWeight(),0.0f,1.0f)*10000);
                value.time=uint32_t(std::max(0.0f,a->GetTimePosition())*1000); value.loop=a->IsLooping();
                if(e.animations.size()<32) e.animations.push_back(value);
            }
        }
        if(auto* mesh=entity->GetMeshEntity()) e.transform=Pack(mesh->GetWorldMatrix()); else e.transform=Pack(cMatrixf::Identity);
        for(int n=0;n<entity->GetBodyNum() && n<32;++n) e.bodies.push_back(Pack(entity->GetBody(n)->GetLocalMatrix()));
        const auto found=published.find(name);
        const bool lifecycle=found==published.end() || found->second.flags!=e.flags || (e.subtype==eGameEntityType_Enemy && ((found->second.health<=0)!=(e.health<=0) || found->second.enemyState!=e.enemyState));
        const bool moved=found==published.end() || found->second.bodies!=e.bodies || found->second.transform!=e.transform || found->second.character!=e.character || !e.animations.empty();
        if(lifecycle || moved) {
            e.sequence=++sequence;
            const bool sent=(baseline || lifecycle)?session.SendEntity(e):session.SendEntityMotion(e);
            if(sent) published[name]=e; else complete=false;
        }
    }
    for(auto it=published.begin();it!=published.end();) {
        if(alive.count(it->first)) { ++it; continue; }
        auto e=it->second; e.flags=4; e.sequence=++sequence;
        if(session.SendEntity(e)) it=published.erase(it); else { complete=false; ++it; }
    }
    return complete;
}
void cCoopNetwork::ApplyEntity(const online::EntityState& s) {
    auto* entity=init->mpMapHandler->GetGameEntity(s.name,false);
    if(!entity && !(s.flags&4) && !s.file.empty()) {
        init->mpGame->GetScene()->GetWorld3D()->CreateEntity(s.name,Unpack(s.transform),s.file,true);
        entity=init->mpMapHandler->GetGameEntity(s.name,false);
        if(entity && entity->GetType()==eGameEntityType_Enemy) entity->OnWorldLoad();
    }
    if(!entity) return;
    if((s.flags&4) && entity->IsActive() && getenv("PENUMBRA_COOP_QA")) Log("[COOP/QA] Replica removed entity=%s t_ms=%llu\n",s.name.c_str(),(unsigned long long)Milliseconds());
    entity->SetActive((s.flags&1)!=0 && !(s.flags&4));
    if(s.flags&4) { motion.erase(s.name); return; }
    if(auto* mesh=entity->GetMeshEntity()) if(s.bodies.empty()) mesh->SetMatrix(Unpack(s.transform));
    if(entity->GetType()==eGameEntityType_Enemy) {
        auto* enemy=static_cast<iGameEnemy*>(entity); enemy->ApplyNetworkState(s.health/10.0f,s.enemyState);
        auto* body=enemy->GetMover()->GetCharBody(); const auto frame=Unpack(s.character);
        body->SetPosition(frame.GetTranslation());
        if(auto* mesh=entity->GetMeshEntity()) {
            if(!s.liveMotion || s.health<=0) mesh->SetMatrix(Unpack(s.transform));
            for(int n=0;n<mesh->GetAnimationStateNum();++n) mesh->GetAnimationState(n)->SetActive(false);
            for(const auto& value:s.animations) if(value.index<mesh->GetAnimationStateNum()) {
                auto* a=mesh->GetAnimationState(value.index); a->SetActive(true); a->SetLoop(value.loop);
                a->SetWeight(value.weight/10000.0f); a->SetTimePosition(value.time/1000.0f);
            }
        }
    }
    Motion m; m.state=s;
    if(s.liveMotion && s.health>0 && entity->GetType()==eGameEntityType_Enemy && entity->GetMeshEntity()) { m.meshMotion=true; m.meshFrom=entity->GetMeshEntity()->GetWorldMatrix(); }
    for(size_t n=0;n<s.bodies.size() && n<size_t(entity->GetBodyNum());++n) {
        auto* body=entity->GetBody(int(n)); m.from.push_back(body->GetLocalMatrix());
        if(!body->IsCharacter()) { body->SetMass(0); body->SetGravity(false); body->SetLinearVelocity(cVector3f(0)); body->SetAngularVelocity(cVector3f(0)); }
        if(!s.liveMotion) body->SetMatrix(Unpack(s.bodies[n]));
    }
    if(s.liveMotion) motion[s.name]=m;
}
void cCoopNetwork::UpdateAvatar(const online::PlayerState& s,float dt) {
    auto* world=init->mpGame->GetScene()->GetWorld3D(); if(!world) return;
    if(!avatar[0]) {
        // Procedural placeholder silhouette: no retail character model is distributed.
        const cVector3f sizes[]={cVector3f(.4f,.65f,.22f),cVector3f(.23f,.23f,.23f),cVector3f(.12f,.55f,.12f),cVector3f(.12f,.55f,.12f),cVector3f(.15f,.7f,.15f),cVector3f(.15f,.7f,.15f)};
        for(int n=0;n<6;++n) {
            std::string name="CoopAvatar_"+std::to_string(n); auto* mesh=init->mpGame->GetGraphics()->GetMeshCreator()->CreateBox(name,sizes[n],"player_gui_hand.mat");
            avatar[n]=world->CreateMeshEntity(name,mesh); avatar[n]->SetIsSaved(false); avatar[n]->SetCastsShadows(true);
        }
        remoteLight=world->CreateLightSpot("CoopPartnerFlashlight","player_flashlight_gobo.tga");
        remoteLight->SetIsSaved(false); remoteLight->SetDiffuseColor(cColor(1,1,.85f,1)); remoteLight->SetFarAttenuation(15); remoteLight->SetFOV(cMath::ToRad(50)); remoteLight->SetCastShadows(true);
        auto* flashlightMesh=init->mpGame->GetResources()->GetMeshManager()->CreateMesh("item_flashlight.dae");
        if(flashlightMesh) { remoteFlashlight=world->CreateMeshEntity("CoopPartnerFlashlightMesh",flashlightMesh); remoteFlashlight->SetIsSaved(false); remoteFlashlight->SetCastsShadows(true); }
        Log("[COOP/AVATAR] Created partner silhouette\n");
    }
    cVector3f target(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f);
    if(!hasAvatarPosition || (avatarPosition-target).SqrLength()>4) avatarPosition=target;
    else avatarPosition+=(target-avatarPosition)*std::min(1.0f,dt*20);
    hasAvatarPosition=true;
    const float crouch=(s.flags&1)?0.7f:1;
    const cVector3f offsets[]={cVector3f(0,1.1f,0),cVector3f(0,1.58f,0),cVector3f(-.27f,1.1f,0),cVector3f(.27f,1.1f,0),cVector3f(-.11f,.35f,0),cVector3f(.11f,.35f,0)};
    for(int n=0;n<6;++n) { auto m=cMath::MatrixRotateY(s.yaw/1000.0f); auto offset=offsets[n]; offset.y*=crouch; m.SetTranslation(avatarPosition+cMath::MatrixMul(m,offset)); avatar[n]->SetMatrix(m); avatar[n]->SetVisible(session.Connected() && !(s.flags&8)); }
    auto lightMatrix=cMath::MatrixMul(cMath::MatrixRotateY(s.yaw/1000.0f),cMath::MatrixRotateX(s.pitch/1000.0f)); lightMatrix.SetTranslation(avatarPosition+cVector3f(0,s.eyeHeight/1000.0f,0));
    remoteLight->SetMatrix(lightMatrix); remoteLight->SetVisible((s.flags&2)!=0);
    if(remoteFlashlight) {
        auto m=cMath::MatrixMul(lightMatrix,cMath::MatrixRotateX(kPi2f));
        m.SetTranslation(lightMatrix.GetTranslation()+cMath::MatrixMul(lightMatrix.GetRotation(),cVector3f(-.23f,-.24f,-.3f)));
        remoteFlashlight->SetMatrix(m); remoteFlashlight->SetVisible(session.Connected() && (s.flags&2) && !(s.flags&8));
    }
    if(s.tinderboxes!=remoteToolID) {
        if(remoteTool) world->DestroyMeshEntity(remoteTool);
        remoteTool=nullptr; remoteToolID=s.tinderboxes;
        if(remoteToolID==1 || remoteToolID==2) {
            auto* mesh=init->mpGame->GetResources()->GetMeshManager()->CreateMesh(remoteToolID==1?"item_hammer.dae":"items_pickaxe.dae");
            if(mesh) { remoteTool=world->CreateMeshEntity("CoopPartnerTool",mesh); remoteTool->SetIsSaved(false); remoteTool->SetCastsShadows(true); }
        }
    }
    toolSwing+=( ((s.flags&64)?1.0f:0.0f)-toolSwing)*std::min(1.0f,dt*25);
    if(remoteTool) {
        auto frame=cMath::MatrixMul(cMath::MatrixRotateY(s.yaw/1000.0f),cMath::MatrixRotateX(s.pitch/1000.0f));
        auto m=cMath::MatrixMul(frame,cMath::MatrixRotateX(-.3f-toolSwing*1.0f));
        m.SetTranslation(avatarPosition+cVector3f(0,s.eyeHeight/1000.0f,0)+cMath::MatrixMul(frame,cVector3f(.23f,-.24f,-.3f)));
        remoteTool->SetMatrix(m); remoteTool->SetVisible(session.Connected() && !(s.flags&8));
    }
}
void cCoopNetwork::RunQA(float dt) {
    if(!getenv("PENUMBRA_COOP_QA") || (!Ready() && qaTime<=0)) return;
    qaTime+=dt;
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="hammer") { RunHammerQA(dt); return; }
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="campaign") { if(hammerTime<10) RunHammerQA(dt); else RunCampaignQA(dt); return; }
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="resume") { RunCampaignQA(dt); return; }
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="intro") {
        auto* song=init->mpGame->GetSound()->GetMusicHandler()->GetCurrentSong();
        if(qaTime>15 && Ready() && init->mpPlayer->IsActive() && song && song->msFileName.find("penumbra_music_E1_A6")!=std::string::npos) qaPuzzle=true;
        const int phase=int(qaTime/2);
        if(qaTime>7 && phase!=qaStage && init->mpGameMessageHandler->HasMessage()) {
            if(IsHost()) init->mpPlayer->StartInteract(); else ClientAction(online::PlayerAction::Interact,true);
            qaStage=phase;
        }
        if(qaTime>22) {
            const bool ok=qaPuzzle;
            Log("[COOP/QA] %s native intro unlock and campaign music role=%s\n",ok?"PASS":"FAIL",IsHost()?"host":"client"); init->mpGame->Exit();
        }
        return;
    }
    const bool full=std::string(getenv("PENUMBRA_COOP_QA"))=="full";
    if(IsHost()) {
        init->mpGameMessageHandler->Reset(); init->mpPlayer->SetActive(true);
        if(qaStage>=2 && qaTime<15) {
            qaGrabbed=qaGrabbed || remotePlayer->GetState()==ePlayerState_Grab;
            if(auto* e=init->mpMapHandler->GetGameEntity(qaObject,false))
                qaObjectMaxDistance=std::max(qaObjectMaxDistance,(e->GetBody(0)->GetLocalPosition()-qaObjectPosition).Length());
        }
        if(qaStage==2 && qaTime<8.6f && remotePlayer->GetState()==ePlayerState_Normal) {
            auto* e=init->mpMapHandler->GetGameEntity(qaObject,false);
            if(e) { auto angles=AimAngles(remotePlayer->GetCamera()->GetPosition(),e->GetBody(0)->GetBV()->GetWorldCenter()); remotePlayer->GetCamera()->SetYaw(angles.y); remotePlayer->GetCamera()->SetPitch(angles.x); remotePlayer->GetCharacterBody()->SetYaw(angles.y); }
        }
        if(qaTime<1) { init->mpFadeHandler->FadeIn(0); init->mpPlayer->GetLookAt()->SetActive(false); init->mpEffectHandler->GetDepthOfField()->SetActive(false,0); }
        if(qaStage==0) { qaInitialPosition=remotePlayer->GetCharacterBody()->GetFeetPosition(); qaStage=1; }
        if(qaStage==1 && qaTime>4) {
            qaMoved=(remotePlayer->GetCharacterBody()->GetFeetPosition()-qaInitialPosition).SqrLength()>.01f && lastInput>30;
            Log("[COOP/QA] Remote movement distance=%.3f input=%u\n",(remotePlayer->GetCharacterBody()->GetFeetPosition()-qaInitialPosition).Length(),lastInput);
            auto it=init->mpMapHandler->GetGameEntityIterator();
            while(it.HasNext()) { auto* e=it.Next(); if(e->GetName().find("whiskey")!=std::string::npos && e->GetBodyNum()) { qaObject=e->GetName(); break; } }
            if(qaObject.empty()) FatalError("[COOP/QA] Missing native whiskey bottle fixture\n");
            auto* e=init->mpMapHandler->GetGameEntity(qaObject,false); qaObjectPosition=e->GetBody(0)->GetLocalPosition();
            cVector3f feet=qaObjectPosition+cVector3f(0,0,1.3f); feet.y=init->mpPlayer->GetCharacterBody()->GetFeetPosition().y;
            remotePlayer->GetCharacterBody()->SetFeetPosition(feet); remotePlayer->GetCharacterBody()->SetMoveSpeed(eCharDir_Forward,0); remotePlayer->GetCharacterBody()->SetMoveSpeed(eCharDir_Right,0);
            auto from=feet+cVector3f(0,remotePlayer->GetSize().y+remotePlayer->GetCameraHeightAdd(),0);
            auto angles=AimAngles(from,e->GetBody(0)->GetBV()->GetWorldCenter());
            remotePlayer->GetCamera()->SetYaw(angles.y); remotePlayer->GetCamera()->SetPitch(angles.x); remotePlayer->GetCharacterBody()->SetYaw(angles.y);
            qaStage=2;
        }
        if(qaStage==2 && qaTime>9) {
            Log("[COOP/QA] Native grab state=%d picked=%s\n",int(remotePlayer->GetState()),remotePlayer->GetPickedBody()?remotePlayer->GetPickedBody()->GetName().c_str():"none");
            qaStage=3;
        }
        if(qaStage==3 && qaTime>13) {
            auto* e=init->mpMapHandler->GetGameEntity(qaObject,false);
            qaReplicated=qaObjectMaxDistance>.08f;
            Log("[COOP/QA] Native grabbed object maximum_distance=%.3f broken=%d released_state=%d\n",qaObjectMaxDistance,e?0:1,int(remotePlayer->GetState())); qaStage=4;
        }
        if(full && qaStage==4 && qaTime>15) {
            auto* locker=init->mpMapHandler->GetGameEntity("padlockarea",false);
            if(!locker || !locker->GetBodyNum()) FatalError("[COOP/QA] Missing locker fixture\n");
            auto target=locker->GetBody(0)->GetLocalPosition();
            auto feet=target+cVector3f(0,0,.8f); feet.y=init->mpPlayer->GetCharacterBody()->GetFeetPosition().y;
            remotePlayer->GetCharacterBody()->SetFeetPosition(feet);
            auto angles=AimAngles(feet+cVector3f(0,remotePlayer->GetSize().y+remotePlayer->GetCameraHeightAdd(),0),target);
            remotePlayer->GetCamera()->SetYaw(angles.y); remotePlayer->GetCamera()->SetPitch(angles.x); remotePlayer->GetCharacterBody()->SetYaw(angles.y); qaStage=5;
        }
        if(full && qaStage==5 && qaTime>21) {
            auto* lock=init->mpMapHandler->GetGameEntity("lock1",false);
            qaPuzzle=init->mpInventory->GetItem("padlock_key")==nullptr && lock && !lock->IsActive();
            Log("[COOP/QA] Native client key puzzle=%d\n",qaPuzzle?1:0); qaStage=6;
        }
        if(full && qaStage==6 && qaTime>24) {
            auto* locker=init->mpMapHandler->GetGameEntity("locker1",false);
            if(!locker || locker->GetBodyNum()<2) FatalError("[COOP/QA] Missing articulated locker fixture\n");
            qaDoorMatrix=locker->GetBody(1)->GetLocalMatrix();
            auto target=locker->GetBody(1)->GetBV()->GetWorldCenter();
            auto feet=target+cVector3f(0,0,.8f); feet.y=init->mpPlayer->GetCharacterBody()->GetFeetPosition().y;
            remotePlayer->GetCharacterBody()->SetFeetPosition(feet);
            auto angles=AimAngles(feet+cVector3f(0,remotePlayer->GetSize().y+remotePlayer->GetCameraHeightAdd(),0),target);
            remotePlayer->GetCamera()->SetYaw(angles.y); remotePlayer->GetCamera()->SetPitch(angles.x); remotePlayer->GetCharacterBody()->SetYaw(angles.y); qaStage=7;
        }
        if(full && qaStage==7 && remotePlayer->GetState()==ePlayerState_Move) qaDoorGrabbed=true;
        if(full && qaStage==7 && qaTime>31) {
            auto* locker=init->mpMapHandler->GetGameEntity("locker1",false); auto a=Pack(qaDoorMatrix), b=Pack(locker->GetBody(1)->GetLocalMatrix());
            for(size_t n=0;n<a.size();++n) if(std::abs(a[n]-b[n])>100) qaDoorMoved=true;
            Log("[COOP/QA] Native client door dragged=%d changed=%d\n",qaDoorGrabbed?1:0,qaDoorMoved?1:0); qaStage=8;
        }
        if(full && qaStage==8 && qaTime>34) {
            init->mpMapHandler->ChangeMap("level01_01_outside.dae","link1","","",.1f,.1f,"",""); qaStage=9;
        }
        if(full && qaStage==9 && map.file=="maps/level01_01_outside.dae" && Ready()) {
            qaTransition=true; Log("[COOP/QA] Native second-map barrier PASS epoch=%u\n",map.epoch); qaStage=10;
        }
    } else {
        if(qaStage==0 && qaTime>5) {
            const auto& s=latest.players[1]; init->mpPlayer->GetCharacterBody()->SetFeetPosition(cVector3f(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f));
            init->mpPlayer->GetCamera()->SetYaw(s.yaw/1000.0f); init->mpPlayer->GetCamera()->SetPitch(s.pitch/1000.0f); qaStage=1;
            auto it=init->mpMapHandler->GetGameEntityIterator(); while(it.HasNext()) { auto* e=it.Next(); if(e->GetName().find("whiskey")!=std::string::npos && e->GetBodyNum()) { qaObject=e->GetName(); qaObjectPosition=e->GetBody(0)->GetLocalPosition(); break; } }
        }
        if(qaStage==1 && qaTime>8) { ClientAction(online::PlayerAction::Interact,true); qaStage=2; }
        if(qaStage==2 && qaTime>9.5f && qaTime<10.5f) init->mpPlayer->GetCamera()->AddYaw(dt*.25f);
        if(qaStage==2 && qaTime>11) { ClientAction(online::PlayerAction::Interact,false); qaStage=3; }
        if(qaStage==3 && qaTime>14) {
            auto* e=init->mpMapHandler->GetGameEntity(qaObject,false); const float distance=e?(e->GetBody(0)->GetLocalPosition()-qaObjectPosition).Length():0;
            qaReplicated=distance>.08f; Log("[COOP/QA] Replica grabbed object distance=%.3f\n",distance); qaStage=4;
        }
        if(full && qaStage==4 && qaTime>17) {
            auto& s=latest.players[1]; init->mpPlayer->GetCamera()->SetYaw(s.yaw/1000.0f); init->mpPlayer->GetCamera()->SetPitch(s.pitch/1000.0f); qaStage=5;
        }
        if(full && qaStage==5 && qaTime>19) {
            auto* key=init->mpInventory->GetItem("padlock_key");
            if(key) { init->mpPlayer->SetCurrentItem(key); init->mpPlayer->ChangeState(ePlayerState_UseItem); init->mpPlayer->StartInteract(); }
            qaStage=6;
        }
        if(full && qaStage==6 && qaTime>23) {
            auto* lock=init->mpMapHandler->GetGameEntity("lock1",false);
            qaPuzzle=init->mpInventory->GetItem("padlock_key")==nullptr && lock && !lock->IsActive();
            Log("[COOP/QA] Replica client key puzzle=%d\n",qaPuzzle?1:0); qaStage=7;
        }
        if(full && qaStage==7 && qaTime>25) {
            auto* locker=init->mpMapHandler->GetGameEntity("locker1",false); qaDoorMatrix=locker->GetBody(1)->GetLocalMatrix();
            auto& s=latest.players[1]; init->mpPlayer->GetCamera()->SetYaw(s.yaw/1000.0f); init->mpPlayer->GetCamera()->SetPitch(s.pitch/1000.0f); qaStage=8;
        }
        if(full && qaStage==8 && qaTime>27) { ClientAction(online::PlayerAction::Interact,true); qaStage=9; }
        if(full && qaStage==9 && qaTime>28 && qaTime<29.5f) init->mpPlayer->AddPitch(dt*2.0f);
        if(full && qaStage==9 && qaTime>30) { ClientAction(online::PlayerAction::Interact,false); qaStage=10; }
        if(full && qaStage==10 && qaTime>32) {
            auto* locker=init->mpMapHandler->GetGameEntity("locker1",false); auto a=Pack(qaDoorMatrix),b=Pack(locker->GetBody(1)->GetLocalMatrix());
            for(size_t n=0;n<a.size();++n) if(std::abs(a[n]-b[n])>100) qaDoorMoved=true;
            Log("[COOP/QA] Replica client door changed=%d\n",qaDoorMoved?1:0); qaStage=11;
        }
        if(full && qaStage==11 && map.file=="maps/level01_01_outside.dae" && Ready() && init->mpGame->GetGraphics()->GetRenderer3D()->GetFogActive() && init->mpGame->GetGraphics()->GetRenderer3D()->GetSkyBoxActive()) {
            qaTransition=true; Log("[COOP/QA] Replica second-map barrier PASS epoch=%u\n",map.epoch); qaStage=12;
        }
    }
    if(qaTime>(full?45:20)) {
        const bool ok=qaReplicated && (!IsHost() || (qaMoved && qaGrabbed)) && (!full || (qaPuzzle && qaTransition && qaDoorMoved && (!IsHost() || qaDoorGrabbed)));
        Log("[COOP/QA] %s connected baseline, movement, native grab and replica object map=%s\n",ok?"PASS":"FAIL",map.file.c_str()); init->mpGame->Exit();
    }
}

void cCoopNetwork::RunHammerQA(float dt) {
    if(IsClient()) init->mbSimpleWeaponSwing=getenv("PENUMBRA_COOP_QA_SIMPLE") && std::string(getenv("PENUMBRA_COOP_QA_SIMPLE"))=="1";
    auto* player=init->mpPlayer;
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    auto aim=[](cPlayer* p,iGameEntity* e) {
        if(!e || !e->GetBodyNum()) return;
        auto angles=AimAngles(p->GetCamera()->GetPosition(),e->GetBody(0)->GetBV()->GetWorldCenter());
        p->GetCamera()->SetYaw(angles.y); p->GetCamera()->SetPitch(angles.x); p->GetCharacterBody()->SetYaw(angles.y);
    };
    auto pickAim=[&](cPlayer* p,iGameEntity* e) {
        auto center=e->GetBody(0)->GetBV()->GetWorldCenter(), size=e->GetBody(0)->GetBV()->GetSize();
        for(int x=-2;x<=2;++x) for(int y=-2;y<=2;++y) for(int z=-2;z<=2;++z) {
            auto target=center+cVector3f(size.x*x*.2f,size.y*y*.2f,size.z*z*.2f);
            auto angles=AimAngles(p->GetCamera()->GetPosition(),target);
            p->GetCamera()->SetYaw(angles.y); p->GetCamera()->SetPitch(angles.x); p->GetCharacterBody()->SetYaw(angles.y);
            auto* ray=p->GetPickRay(); ray->Clear(); auto start=p->GetCamera()->GetPosition();
            world->GetPhysicsWorld()->CastRay(ray,start,start+p->GetCamera()->GetForward()*ray->mfMaxDistance,true,false,true);
            ray->CalculateResults();
            if(p->GetPickedBody()==e->GetBody(0) && ray->mfPickedDist<=e->GetMaxInteractDist()) return true;
        }
        return false;
    };
    if(IsHost()) {
        init->mpGameMessageHandler->Reset(); player->SetActive(true);
        player->GetLookAt()->SetActive(false); if(qaTime<1 || qaStage==2) init->mpFadeHandler->FadeIn(0); init->mpEffectHandler->GetDepthOfField()->SetActive(false,0);
        if(qaStage==0 && Ready() && qaTime>.8f) {
            // Collect the real cabin flashlight through its native pickup path.
            if(auto* light=init->mpMapHandler->GetGameEntity("flashlight",false)) {
                auto target=light->GetBody(0)->GetBV()->GetWorldCenter();
                player->GetCharacterBody()->SetFeetPosition(target+cVector3f(0,-.6f,.5f));
                player->SetPickedBody(light->GetBody(0)); player->GetPickRay()->mvPickedPos=target; light->PlayerInteract();
            }
            init->mpMapHandler->ChangeMap("level01_01_outside.dae","link1","","",.1f,.1f,"",""); qaStage=1;
        }
        if(qaStage==1 && map.file=="maps/level01_01_outside.dae" && Ready() && qaTime>4) {
            // Fixture opens the hatch gate; the original exit interaction changes the map.
            auto* exit=init->mpMapHandler->GetGameEntity("exitarea",false);
            if(!exit || !exit->GetBodyNum()) FatalError("[COOP/QA] Missing outside hatch exit\n");
            auto target=exit->GetBody(0)->GetBV()->GetWorldCenter(); exit->SetActive(true);
            player->GetCharacterBody()->SetFeetPosition(target+cVector3f(0,-.5f,.1f));
            player->SetPickedBody(exit->GetBody(0)); player->GetPickRay()->mvPickedPos=target; exit->PlayerInteract(); qaStage=2;
        }
    }
    if(map.file!="maps/level01_02_mine_entrance.dae" || (!Ready() && hammerTime==0)) return;
    hammerTime+=dt;
    if(hammerTime>9 && !hammerShared) {
        Log("[COOP/QA] FAIL native hammer pickup role=%s\n",IsHost()?"host":"client"); init->mpGame->Exit(); return;
    }
    init->mpGameMessageHandler->Reset();
    if(player->GetState()==ePlayerState_Message) player->ChangeState(ePlayerState_Normal);
    if(IsHost()) {
        if(qaStage==2) {
            auto* hammer=init->mpMapHandler->GetGameEntity("hammer",false);
            if(!hammer) FatalError("[COOP/QA] Missing real mine hammer\n");
            auto target=hammer->GetBody(0)->GetBV()->GetWorldCenter();
            qaInitialPosition=remotePlayer->GetCharacterBody()->GetFeetPosition();
            remotePlayer->ChangeMoveState(ePlayerMoveState_Walk,true);
            bool accessible=false;
            for(int side=0;side<16 && !accessible;++side) {
                const float angle=side*kPi2f/2, radius=side<8?.8f:1.1f;
                cVector3f feet(target.x+std::cos(angle)*radius,.3f,target.z+std::sin(angle)*radius);
                auto matrix=cMatrixf::Identity; matrix.SetTranslation(feet+cVector3f(0,remotePlayer->GetSize().y/2,0));
                const bool blocked=world->GetPhysicsWorld()->CheckShapeWorldCollision(nullptr,remotePlayer->GetCharacterBody()->GetShape(),matrix,remotePlayer->GetCharacterBody()->GetBody(),false,true,nullptr,false);
                remotePlayer->GetCamera()->SetPosition(feet+cVector3f(0,remotePlayer->GetSize().y+remotePlayer->GetCameraHeightAdd()+remotePlayer->GetHeightAdd(),0));
                const bool seen=pickAim(remotePlayer,hammer);
                Log("[COOP/QA] Hammer view side=%d blocked=%d seen=%d center=%.3f/%.3f/%.3f size=%.3f/%.3f/%.3f\n",side,blocked,seen,target.x,target.y,target.z,hammer->GetBody(0)->GetBV()->GetSize().x,hammer->GetBody(0)->GetBV()->GetSize().y,hammer->GetBody(0)->GetBV()->GetSize().z);
                if(seen && !blocked) { remotePlayer->GetCharacterBody()->SetFeetPosition(feet); accessible=true; }
            }
            if(!accessible) { Log("[COOP/QA] FAIL no unobstructed native hammer pickup view\n"); init->mpGame->Exit(); return; }
            qaStage=3; Log("[COOP/QA] Arrived in hammer level via cabin/outside/hatch epoch=%u\n",map.epoch);
        }
        qaAimLocked=hammerTime<6 || (hammerTime>10 && hammerTime<25);
        if(hammerTime<6) {
            auto* hammer=init->mpMapHandler->GetGameEntity("hammer",false);
            // Aim at a solid part of the thin hammer rather than empty space in its AABB.
            if(hammer && !pickAim(remotePlayer,hammer)) aim(remotePlayer,hammer);
        }
        if(hammerTime>6 && !hammerShared) {
            hammerShared=init->mpInventory->GetItem("hammer")!=nullptr;
            if(hammerShared) { Log("[COOP/QA] Native hammer pickup shared on host\n"); init->mpInventory->GetItemType(eGameItemType_WeaponMelee)->OnAction(init->mpInventory->GetItem("hammer"),0); }
        }
        if(hammerTime>8 && hammerTime<8+dt*2 && !player->GetFlashLight()->IsActive()) player->StartFlashLightButton();
        if(hammerTime>10 && qaStage==3) {
            hammerEquipped=player->GetState()==ePlayerState_WeaponMelee && remotePlayer->GetState()==ePlayerState_WeaponMelee;
            lightsSeen=player->GetFlashLight()->IsActive() && remotePlayer->GetFlashLight()->IsActive() && remoteLight && remoteLight->IsVisible() && remoteFlashlight && remoteFlashlight->IsVisible();
            if(remoteLight) {
                const auto direction=cMath::MatrixMul(remoteLight->GetWorldMatrix().GetRotation(),cVector3f(0,0,-1));
                const float alignment=cMath::Vector3Dot(direction,remotePlayer->GetCamera()->GetForward());
                lightsSeen=lightsSeen && alignment>.99f;
                Log("[COOP/QA] Partner flashlight native aim alignment=%.5f\n",alignment);
            }
            Log("[COOP/QA] Both native hammer states=%d flashlight pair=%d shared_flashlight=%d power=%.1f/%.1f lights=%d/%d/%d\n",hammerEquipped?1:0,lightsSeen?1:0,init->mpInventory->GetItem("flashlight")?1:0,player->GetPower(),remotePlayer->GetPower(),player->GetFlashLight()->IsActive(),remotePlayer->GetFlashLight()->IsActive(),remoteLight?remoteLight->IsVisible():0);
            auto matrix=cMatrixf::Identity; matrix.SetTranslation(cVector3f(0,.55f,6.6f));
            world->CreateEntity("CoopQA_ClientCrate",matrix,"modern_mine_woodcrate.ent",true);
            matrix.SetTranslation(cVector3f(0,.55f,3.8f)); world->CreateEntity("CoopQA_HostCrate",matrix,"modern_mine_woodcrate.ent",true);
            remotePlayer->GetCharacterBody()->SetFeetPosition(cVector3f(0,0,7.5f));
            player->GetCharacterBody()->SetFeetPosition(cVector3f(0,0,4.7f));
            player->GetCamera()->SetPosition(player->GetCharacterBody()->GetFeetPosition()+cVector3f(0,player->GetSize().y+player->GetCameraHeightAdd()+player->GetHeightAdd(),0));
            player->ChangeMoveState(ePlayerMoveState_Walk,true);
            auto view=AimAngles(player->GetCamera()->GetPosition(),remotePlayer->GetCharacterBody()->GetFeetPosition()+cVector3f(0,1.5f,0));
            player->GetCamera()->SetYaw(view.y); player->GetCamera()->SetPitch(view.x);
            qaStage=4;
        }
        if(hammerTime>10 && hammerTime<25) aim(remotePlayer,init->mpMapHandler->GetGameEntity("CoopQA_ClientCrate",false));
        if(hammerTime>26 && hammerTime<39) {
            aim(player,init->mpMapHandler->GetGameEntity("CoopQA_HostCrate",false));
            const float t=hammerTime-26, phase=std::fmod(t,1.25f); const int cycle=int(t/1.25f);
            if(cycle!=hammerCycle) { hammerCycle=cycle; player->StopInteract(); player->StartInteract(); }
            if(phase>.1f && phase<.25f) player->AddYaw(.08f);
            if(phase>.5f && phase<.65f) player->AddYaw(-.08f);
            if(phase>1) player->StopInteract();
        }
        if(hammerTime>39 && qaStage==4) {
            player->StopInteract();
            auto* a=init->mpMapHandler->GetGameEntity("CoopQA_ClientCrate_broken",false);
            auto* b=init->mpMapHandler->GetGameEntity("CoopQA_HostCrate_broken",false);
            clientBroke=a && a->IsActive() && clientHits>=7; hostBroke=b && b->IsActive() && hostHits>=7;
            Log("[COOP/QA] Native crate destruction client=%d host=%d client_hits=%u host_hits=%u\n",clientBroke?1:0,hostBroke?1:0,clientHits,hostHits);
            {
                PlayerScope scope(init,remotePlayer,remoteHands); remotePlayer->StartHolster();
                auto matrix=cMatrixf::Identity; matrix.SetTranslation(cVector3f(0,.55f,6.6f));
                world->CreateEntity("CoopQA_HeldCrate",matrix,"modern_mine_woodcrate.ent",true);
                auto* held=init->mpMapHandler->GetGameEntity("CoopQA_HeldCrate",false);
                if(held) {
                    remotePlayer->SetPickedBody(held->GetBody(0)); remotePlayer->GetPickRay()->mvPickedPos=held->GetBody(0)->GetBV()->GetWorldCenter();
                    held->PlayerInteract(); const auto holding=remotePlayer->GetState();
                    heldCrateStarted=holding==ePlayerState_Grab || holding==ePlayerState_Move || holding==ePlayerState_Push;
                    Log("[COOP/QA] Held crate native state=%d distance=%.3f maximum=%.3f\n",int(holding),remotePlayer->GetPickedDist(),held->GetMaxInteractDist());
                    held->Damage(100,10);
                }
            }
            // Exercise removal before a later input release can mask the held-body case.
            init->mpMapHandler->Update(0);
            qaStage=5;
        }
        if(hammerTime>39.5f && qaStage==5) {
            heldCrateReleased=heldCrateStarted && remotePlayer->GetState()==ePlayerState_Normal && init->mpMapHandler->GetGameEntity("CoopQA_HeldCrate_broken",false);
            Log("[COOP/QA] Held crate safely released before destruction=%d\n",heldCrateReleased?1:0);
            remotePlayer->GetCharacterBody()->SetFeetPosition(qaInitialPosition);
            remotePlayer->GetCamera()->SetYaw(0); remotePlayer->GetCamera()->SetPitch(0); remotePlayer->GetCharacterBody()->SetYaw(0); qaStage=6;
        }
    } else {
        if(hammerTime>10 && hammerTime<25) { player->GetCamera()->SetYaw(latest.players[1].yaw/1000.0f); player->GetCamera()->SetPitch(latest.players[1].pitch/1000.0f); }
        if(hammerTime<6) {
            const auto& s=latest.players[1]; player->GetCamera()->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetPitch(s.pitch/1000.0f);
        }
        if(hammerTime>4 && qaStage==0) { player->StartInteract(); qaStage=1; }
        if(hammerTime>4.5f && qaStage==1) { player->StopInteract(); qaStage=2; }
        if(hammerTime>7 && qaStage==2) {
            auto* item=init->mpInventory->GetItem("hammer"); hammerShared=item!=nullptr;
            if(item) init->mpInventory->GetItemType(item->GetItemType())->OnAction(item,0);
            if(!player->GetFlashLight()->IsActive()) player->StartFlashLightButton();
            qaStage=3;
        }
        if(hammerTime>10 && qaStage==3) {
            hammerEquipped=player->GetState()==ePlayerState_WeaponMelee && latest.players[0].tinderboxes==1 && latest.players[1].tinderboxes==1;
            lightsSeen=player->GetFlashLight()->IsActive() && remoteLight && remoteLight->IsVisible() && remoteFlashlight && remoteFlashlight->IsVisible();
            Log("[COOP/QA] Replica shared hammer=%d both equipped=%d flashlight pair=%d\n",hammerShared?1:0,hammerEquipped?1:0,lightsSeen?1:0); qaStage=4;
        }
        if(hammerTime>12 && hammerTime<25) {
            player->GetCamera()->SetYaw(latest.players[1].yaw/1000.0f); player->GetCamera()->SetPitch(latest.players[1].pitch/1000.0f);
            const float t=hammerTime-12, phase=std::fmod(t,1.25f); const int cycle=int(t/1.25f);
            if(cycle!=hammerCycle) { hammerCycle=cycle; player->StopInteract(); player->StartInteract(); }
            if(phase>.1f && phase<.25f) player->AddYaw(.08f);
            if(phase>.5f && phase<.65f) player->AddYaw(-.08f);
            if(phase>1) player->StopInteract();
        }
        if(hammerTime>39 && qaStage==4) {
            player->StopInteract(); player->StartHolster();
            auto* a=init->mpMapHandler->GetGameEntity("CoopQA_ClientCrate",false); auto* b=init->mpMapHandler->GetGameEntity("CoopQA_HostCrate",false);
            auto* ab=init->mpMapHandler->GetGameEntity("CoopQA_ClientCrate_broken",false); auto* bb=init->mpMapHandler->GetGameEntity("CoopQA_HostCrate_broken",false);
            clientBroke=(!a || !a->IsActive()) && ab && ab->IsActive(); hostBroke=(!b || !b->IsActive()) && bb && bb->IsActive();
            Log("[COOP/QA] Replica crate destruction client=%d host=%d fragments=%d/%d\n",clientBroke?1:0,hostBroke?1:0,ab?ab->GetBodyNum():0,bb?bb->GetBodyNum():0);
            player->GetCamera()->SetYaw(0); player->GetCamera()->SetPitch(0); qaStage=5;
        }
    }
    if(hammerTime>49 && hammerTime<51) {
        if(player->GetState()!=ePlayerState_WeaponMelee) {
            if(auto* item=init->mpInventory->GetItem("hammer")) init->mpInventory->GetItemType(item->GetItemType())->OnAction(item,0);
        }
        const auto& partner=latest.players[IsHost()?1:0];
        auto angles=AimAngles(player->GetCamera()->GetPosition(),cVector3f(partner.x/1000.0f,(partner.y+partner.eyeHeight)/1000.0f,partner.z/1000.0f));
        player->GetCamera()->SetYaw(angles.y); player->GetCamera()->SetPitch(angles.x); player->GetCharacterBody()->SetYaw(angles.y);
    }
    if(hammerTime>(IsHost()?56:52)) {
        if(IsClient()) Log("[COOP/QA] Movement intervals average_ms=%.2f max_ms=%.2f samples=%u correction_max_m=%.4f max_horizontal_step_m=%.4f path_m=%.3f hard_corrections=%u\n",intervalCount?intervalSum*1000/intervalCount:0,intervalMax*1000,intervalCount,correctionMax,stepMax,movementDistance,hardCorrections);
        const bool smooth=IsHost() || (movementDistance>.5f && stepMax<.2f && hardCorrections==0 && intervalMax<.2f);
        const bool ok=hammerShared && hammerEquipped && clientBroke && hostBroke && lightsSeen && smooth && (IsClient() || heldCrateReleased);
        Log("[COOP/QA] %s hammer, independent equipment, both crate attackers and flashlight pair role=%s\n",ok?"PASS":"FAIL",IsHost()?"host":"client"); init->mpGame->Exit();
    }
}

void cCoopNetwork::OnStart(cSoundEntity* sound) {
    if(!IsHost() || !session.Connected() || !sound || !sound->GetData()) return;
    online::MediaState s; s.kind=online::MediaKind::Sound; s.id=EntityID(sound->GetName()); s.name=sound->GetName(); s.file=sound->GetData()->GetName(); s.flags=1;
    s.transform=Pack(sound->GetWorldMatrix()); s.values[0]=int32_t(sound->GetVolume()*1000);
    if(pendingMedia.size()<256) pendingMedia.push_back(s);
}
void cCoopNetwork::PublishMedia() {
    PublishEnvironment();
    for(auto it=pendingMedia.begin();it!=pendingMedia.end();) {
        it->epoch=map.epoch;
        if(session.SendMedia(*it)) it=pendingMedia.erase(it); else break;
    }
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    auto sounds=world->GetSoundEntityIterator();
    while(sounds.HasNext()) {
        auto* sound=sounds.Next(); online::MediaState s; s.epoch=map.epoch; s.id=EntityID(sound->GetName()); s.kind=online::MediaKind::Sound;
        s.name=sound->GetName(); s.file=sound->GetData()->GetName(); s.flags=sound->IsStopped()?0:1;
        s.transform=Pack(sound->GetWorldMatrix()); s.values[0]=int32_t(sound->GetVolume()*1000);
        auto bytes=online::EncodeMedia(s); auto key="s/"+s.name;
        if(mediaBytes[key]!=bytes && session.SendMedia(s)) mediaBytes[key]=bytes;
    }
    auto* song=init->mpGame->GetSound()->GetMusicHandler()->GetCurrentSong();
    online::MediaState s; s.epoch=map.epoch; s.id=1; s.kind=online::MediaKind::Music; s.name="campaign_music";
    if(song) { s.file=song->msFileName; s.flags=(song->mbLoop?2:0)|1; s.values[0]=int32_t(song->mfMaxVolume*1000); } else s.flags=128;
    auto bytes=online::EncodeMedia(s);
    if(mediaBytes["music"]!=bytes && session.SendMedia(s)) mediaBytes["music"]=bytes;
}
void cCoopNetwork::PublishEnvironment() {
    auto* r=init->mpGame->GetGraphics()->GetRenderer3D();
    auto send=[&](online::Presentation e,const std::string& key) {
        e.epoch=map.epoch; auto bytes=online::EncodePresentation(e);
        if(mediaBytes[key]!=bytes && session.SendPresentation(e)) mediaBytes[key]=bytes;
    };
    auto colored=[&](online::PresentationKind kind,const cColor& color,const std::string& key) {
        online::Presentation e; e.kind=kind; e.text[0]=key;
        e.values[0]=int32_t(color.r*1000); e.values[1]=int32_t(color.g*1000); e.values[2]=int32_t(color.b*1000); e.values[3]=int32_t(color.a*1000); return e;
    };
    auto sky=colored(online::PresentationKind::Light,r->GetSkyBoxColor(),"coop_skybox");
    if(r->GetSkyBox()) sky.text[1]=r->GetSkyBox()->GetName(); sky.values[4]=r->GetSkyBoxActive()?1:0; send(sky,"env/sky");
    send(colored(online::PresentationKind::Light,r->GetAmbientColor(),"coop_ambient"),"env/ambient");
    send(colored(online::PresentationKind::FogColor,r->GetFogColor(),"coop_fog"),"env/fog");
    online::Presentation fog; fog.kind=online::PresentationKind::FogProperties; fog.values[0]=int32_t(r->GetFogStart()*1000); fog.values[1]=int32_t(r->GetFogEnd()*1000); fog.values[3]=(r->GetFogActive()?1:0)|(r->GetFogCulling()?2:0); send(fog,"env/props");
}
void cCoopNetwork::ApplyMedia(const online::MediaState& s) {
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    if(s.kind==online::MediaKind::Particle && (s.flags&1) && !world->GetParticleSystem(s.name))
        world->CreateParticleSystem(s.name,s.file,cVector3f(1,1,1),Unpack(s.transform));
    if(s.kind==online::MediaKind::Sound) {
        auto* sound=world->GetSoundEntity(s.name);
        if(!sound && (s.flags&1)) sound=world->CreateSoundEntity(s.name,s.file,false);
        if(!sound) return;
        sound->SetMatrix(Unpack(s.transform)); sound->SetVolume(s.values[0]/1000.0f);
        if((s.flags&1) && sound->IsStopped()) sound->Play(false);
        if(!(s.flags&1) && !sound->IsStopped()) sound->Stop(false);
    }
    if(s.kind==online::MediaKind::Music) {
        auto* music=init->mpGame->GetSound()->GetMusicHandler();
        if(s.flags&128) { music->Stop(1); currentMusic.clear(); }
        else if(!s.file.empty() && currentMusic!=s.file) { music->Play(s.file,s.values[0]/1000.0f,1,(s.flags&2)!=0); currentMusic=s.file; }
    }
}
void cCoopNetwork::OnPostSceneDraw() {
    if(!getenv("PENUMBRA_COOP_QA") || !Ready()) return;
    const char* path=getenv("PENUMBRA_COOP_SCREENSHOT");
    if(!path || !*path) return;
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="hammer") {
        auto save=[&](uint8_t bit,const std::string& suffix) {
            if(qaScreenshots&bit) return;
            std::string name(path); name=name.substr(0,name.find_last_of('.'))+suffix+".bmp";
            init->mpGame->GetGraphics()->GetLowLevel()->SaveScreenToBMP(name); qaScreenshots|=bit;
        };
        if(hammerTime>2.5f) save(8,"_pickup");
        if(hammerTime>10.5f) save(1,"_hammer");
        if(hammerTime>25) save(2,"_client_crate");
        if(hammerTime>41) save(4,"_both_crates");
        if(hammerTime>50) save(16,"_pair");
        return;
    }
    if(qaTime<15) return;
    if(!(qaScreenshots&1)) { init->mpGame->GetGraphics()->GetLowLevel()->SaveScreenToBMP(path); qaScreenshots|=1; }
    if(qaTime>32 && map.file=="maps/level01_01_outside.dae" && !(qaScreenshots&2) &&
       (IsHost() || (init->mpGame->GetGraphics()->GetRenderer3D()->GetFogActive() && init->mpGame->GetGraphics()->GetRenderer3D()->GetSkyBoxActive()))) {
        std::string second(path); second=second.substr(0,second.find_last_of('.'))+"_outside.bmp";
        init->mpGame->GetGraphics()->GetLowLevel()->SaveScreenToBMP(second); qaScreenshots|=2;
    }
}
