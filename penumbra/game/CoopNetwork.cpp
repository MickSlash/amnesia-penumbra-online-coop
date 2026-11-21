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
#include "ButtonHandler.h"
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
#include <unordered_map>

using namespace hpl;
namespace hpl { extern bool gbGameScriptExecution; }
namespace hpl { extern bool gbQuitRequested; extern bool gbPhysicsSoundsMuted; }
namespace hpl { extern bool (*gGameGuiSoundCallback)(const tString&,float); }
namespace {
cCoopNetwork* activeNetwork=nullptr;
uint64_t Milliseconds() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
// Solo ASCII: i testi del motore passano per mbstowcs con la locale "C".
std::string AsciiText(const tWString& text) {
    std::string shown;
    for(wchar_t c:text) {
        if(c<128) shown+=char(c);
        else if(c>=0xE0 && c<=0xE4) shown+='a'; else if(c>=0xE8 && c<=0xEB) shown+='e';
        else if(c>=0xEC && c<=0xEF) shown+='i'; else if(c>=0xF2 && c<=0xF6) shown+='o';
        else if(c>=0xF9 && c<=0xFC) shown+='u'; else if(c>=0xC0 && c<=0xC4) shown+='A';
        else if(c>=0xC8 && c<=0xCB) shown+='E'; else shown+='?';
    }
    return shown;
}
// Avvisi nella lingua del gioco dell'host (come i nomi di oggetti e note).
bool ItalianGame(cInit* init) { return cString::ToLowerCase(init->msLanguageFile).find("italian")!=tString::npos; }
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
// Guida a velocità dei corpi replicati sul client, adattata da QuasiInteractive
// (penumbra-overture-coop, Overture/multiplayer/BodySync.cpp, GPLv3).
// Invece di posizionare ogni tick un corpo statico (SetMatrix non controlla le
// collisioni), il corpo resta dinamico e riceve la velocità che chiude
// l'errore verso la posa riprodotta dell'host: Newton gestisce i contatti,
// quindi niente compenetrazioni, e il personaggio del client può spingerlo
// subito come sull'host. Scatto solo per errori grandi o corpo incastrato;
// a riposo il corpo viene bloccato esattamente nella posa dell'host.
// Client: massa originale dei corpi replicati messi a massa 0 (vedi CoopPickMass).
std::unordered_map<const iPhysicsBody*,float> gReplicaMass;
bool BodyDriveEnabled() { const char* v=getenv("PENUMBRA_COOP_BODY_DRIVE"); return v && std::string(v)=="1"; }
float VectorLength(const cVector3f& v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
cVector3f ClampLength(const cVector3f& v,float limit) { const float l=VectorLength(v); return l>limit && l>0 ? v*(limit/l) : v; }
// Asse e angolo della rotazione nel mondo che porta l'orientamento attuale su quello obiettivo.
void RotationError(const cMatrixf& target,const cMatrixf& current,cVector3f& axis,float& angle) {
    float e[3][3];
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) e[i][j]=target.m[i][0]*current.m[j][0]+target.m[i][1]*current.m[j][1]+target.m[i][2]*current.m[j][2];
    const cVector3f skew(e[2][1]-e[1][2],e[0][2]-e[2][0],e[1][0]-e[0][1]);
    const float sine=VectorLength(skew);
    const float cosine=std::clamp(0.5f*(e[0][0]+e[1][1]+e[2][2]-1.0f),-1.0f,1.0f);
    angle=std::atan2(0.5f*sine,cosine); axis=sine>1e-6f?skew/sine:cVector3f(0,0,0);
}
void PinBody(iPhysicsBody* body,const cMatrixf& target,cCoopNetwork::DriveState& state) {
    body->SetMatrix(target); body->SetLinearVelocity(cVector3f(0,0,0)); body->SetAngularVelocity(cVector3f(0,0,0));
    body->SetEnabled(false); state.pinned=true; state.stuck=state.disturbed=0;
}
// Porta il corpo verso `target`; `resting` = l'host non lo muove più.
void DriveBody(iPhysicsBody* body,const cMatrixf& target,bool resting,cCoopNetwork::DriveState& state,float dt,const cVector3f& gravity) {
    const bool jointed=body->GetJointNum()>0;
    // Velocità dell'obiettivo stimata fra due tick (feed-forward).
    cVector3f feed(0,0,0), feedOmega(0,0,0);
    if(state.hasTarget && dt>0 && !resting) {
        feed=ClampLength((target.GetTranslation()-state.lastTarget.GetTranslation())/dt,20.0f);
        cVector3f axis; float angle; RotationError(target,state.lastTarget,axis,angle); feedOmega=ClampLength(axis*(angle/dt),25.0f);
    }
    state.lastTarget=target; state.hasTarget=true;
    if(state.pinned) {
        if(!resting) state.pinned=false;
        else if(!body->GetEnabled()) { state.disturbed=0; return; }
        else { state.disturbed+=dt; if(state.disturbed<0.5f) return; state.pinned=false; }
    }
    const cMatrixf current=body->GetWorldMatrix(); const cVector3f centre=body->GetMassCentre();
    const cVector3f error=cMath::MatrixMul(target,centre)-cMath::MatrixMul(current,centre);
    const float distance=VectorLength(error); cVector3f axis; float angle; RotationError(target,current,axis,angle);
    bool snap=distance>1.5f || angle>2.6f;
    if(distance>0.35f || angle>1.0f) state.stuck+=dt; else state.stuck=0;
    if(state.stuck>(jointed?1.5f:0.5f)) snap=true;
    if(resting && (snap || (distance<0.02f && angle<0.035f))) { PinBody(body,target,state); return; }
    if(snap) { body->SetMatrix(target); body->SetLinearVelocity(cVector3f(0,0,0)); body->SetAngularVelocity(cVector3f(0,0,0)); body->SetEnabled(true); state.stuck=0; return; }
    cVector3f velocity=ClampLength(error*(1.0f/0.1f)+feed,jointed?4.0f:10.0f);
    const cVector3f omega=ClampLength(axis*(angle/0.1f)+feedOmega,jointed?6.0f:20.0f);
    if(!body->GetEnabled()) {
        if(distance<0.005f && angle<0.009f && VectorLength(feed)<0.05f && VectorLength(feedOmega)<0.05f) return;
        body->SetEnabled(true);
    }
    if(body->GetGravity()) velocity-=gravity;
    body->SetLinearVelocity(velocity); body->SetAngularVelocity(omega);
}
bool KinematicPrediction() { const char* mode=getenv("PENUMBRA_COOP_PREDICTION"); return mode && std::string(mode)=="kinematic"; }
struct PlayerScope {
    cInit* init; cPlayer* player; cPlayerHands* hands; bool simple; cInventory* inventory;
    PlayerScope(cInit* i, cPlayer* p, cPlayerHands* h) : init(i), player(i->mpPlayer), hands(i->mpPlayerHands), simple(i->mbSimpleWeaponSwing), inventory(i->mpInventory) {
        init->mpPlayer = p; init->mpPlayerHands = h;
        if(p && p->IsNetworkRemote() && i->mpCoop) init->mbSimpleWeaponSwing=i->mpCoop->RemoteSimpleSwing();
        // Inventario per giocatore: chi agisce usa il proprio.
        if(i->mpCoop) init->mpInventory=i->mpCoop->InventoryFor(p);
    }
    ~PlayerScope() { init->mpPlayer = player; init->mpPlayerHands = hands; init->mbSimpleWeaponSwing=simple; init->mpInventory=inventory; }
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
online::PlayerState Capture(cPlayer* p,cPlayerHands* hands,uint8_t id) {
    online::PlayerState s; s.id=id; auto* body=p->GetCharacterBody(); auto* camera=p->GetCamera();
    cVector3f pos=body->GetFeetPosition(); s.x=int32_t(std::round(pos.x*1000)); s.y=int32_t(std::round(pos.y*1000)); s.z=int32_t(std::round(pos.z*1000));
    s.yaw=int16_t(Wrapped(camera->GetYaw())*1000); s.pitch=int16_t(camera->GetPitch()*1000);
    s.health=uint16_t(std::clamp(p->GetHealth(),0.0f,100.0f)*100); s.oil=uint16_t(std::clamp(p->GetPower(),0.0f,100.0f)*100);
    s.eyeHeight=int16_t((camera->GetPosition().y-pos.y)*1000);
    s.flags=(p->IsActive()?4:0)|(p->IsDead()?8:0)|(p->GetMoveState()==ePlayerMoveState_Crouch?1:0)|(p->GetFlashLight()->IsActive()?2:0);
    if(p->GetState()==ePlayerState_Move) s.flags|=16;
    const auto state=p->GetState();
    const online::HeldMode held=state==ePlayerState_Grab?online::HeldMode::Grab:state==ePlayerState_Move?online::HeldMode::Move:state==ePlayerState_Push?online::HeldMode::Push:online::HeldMode::None;
    if(held!=online::HeldMode::None && p->GetPushBody()) {
        if(auto* entity=static_cast<iGameEntity*>(p->GetPushBody()->GetUserData())) {
            s.heldEntity=EntityID(entity->GetName()); s.heldMode=uint8_t(held);
            for(int n=0;n<entity->GetBodyNum() && n<255;++n) if(entity->GetBody(n)==p->GetPushBody()) s.heldBody=uint8_t(n);
        }
    }
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

cCoopNetwork::cCoopNetwork(cInit* i) : iUpdateable("OvertureOnline"), init(i), transport([](const std::string& s){ Log("[COOP] %s\n",s.c_str()); }) {
    hostPlayer=init->mpPlayer; hostHands=init->mpPlayerHands; mainInventory=init->mpInventory;
    auto logger=[](uint8_t id) {
        // Il primo ospite mantiene il prefisso storico, che le prove leggono.
        const std::string prefix=id<=2?"[COOP]":"[COOP/P"+std::to_string(id)+"]";
        return [prefix](const std::string& s){ Log("%s %s\n",prefix.c_str(),s.c_str()); };
    };
    guests.push_back(std::make_unique<Guest>(logger(2))); Bind(*guests[0]);
    const char* role=getenv("PENUMBRA_COOP_ROLE"); if(!role) return;
    const char* portText=getenv("PENUMBRA_COOP_PORT"); const long port=portText?std::strtol(portText,nullptr,10):online::DefaultPort;
    if(port<1 || port>65535) FatalError("Invalid cooperative UDP port\n");
    if(std::string(role)=="host") {
        // Giocatori in totale, host compreso (2..4).
        const char* playersText=getenv("PENUMBRA_COOP_PLAYERS");
        const long players=playersText?std::strtol(playersText,nullptr,10):2;
        if(players<2 || players>long(online::MaxPlayers)) FatalError("Invalid cooperative player count\n");
        while(guests.size()+1<size_t(players)) guests.push_back(std::make_unique<Guest>(logger(uint8_t(guests.size()+2))));
        std::vector<online::Session*> sessions; for(auto& guest:guests) sessions.push_back(&guest->session);
        if(!transport.Start(uint16_t(port),sessions)) FatalError("Cannot open cooperative UDP port\n");
        hosting=true;
        for(auto& guest:guests) CreateRemote(*guest);
        Bind(*guests[0]);
    }
    else if(std::string(role)=="client") {
        const char* address=getenv("PENUMBRA_COOP_ADDRESS");
        if(!address || !*address || !g->session.StartClient(address,uint16_t(port))) FatalError("Cannot connect to cooperative host\n");
        hpl::gbGameScriptExecution=false;
    } else FatalError("Invalid cooperative role\n");
    Log("[COOP] Experimental Overture protocol %u; campaign authority=host; players=%u\n",unsigned(online::ProtocolVersion),unsigned(guests.size()+1));
    if(IsHost()) cSoundEntity::AddGlobalCallback(this);
    activeNetwork=this; hpl::gGameGuiSoundCallback=ScriptSound;
    steam.Start();
}
cCoopNetwork::~cCoopNetwork() {
    hpl::gGameGuiSoundCallback=nullptr; activeNetwork=nullptr;
    cSoundEntity::RemoveGlobalCallback(this);
    for(auto& guest:guests) { guest->session.CloseVoluntarily(); guest->session.Stop(); }
    transport.Stop();
    for(auto& guest:guests) if(guest->player) {
        PlayerScope scope(init,guest->player,guest->hands);
        if(guest->player->GetCharacterBody()) guest->player->OnWorldExit();
        guest->hands->OnWorldExit();
        delete guest->player; delete guest->hands;
        guest->player=nullptr; guest->hands=nullptr;
    }
    remotePlayer=nullptr; remoteHands=nullptr;
    for(auto& guest:guests) if(guest->ownInventory) { hplDelete(guest->ownInventory); guest->ownInventory=nullptr; }
    hpl::gbGameScriptExecution=true;
}
void cCoopNetwork::CreateRemote(Guest& guest) {
    guest.hands=new cPlayerHands(init);
    { PlayerScope scope(init,init->mpPlayer,guest.hands); guest.player=new cPlayer(init); guest.player->SetNetworkRemote(true); }
    // Il proprio inventario; le callback degli script restano quelle dell'host.
    if(!guest.ownInventory) { guest.ownInventory=hplNew(cInventory,(init)); guest.ownInventory->SetCallbackOwner(mainInventory); }
    GiveGroupItems(guest.player,guest.hands);
    init->mpGame->GetScene()->SetCamera(init->mpPlayer->GetCamera());
}
cPlayerHands* cCoopNetwork::HandsFor(const cPlayer* player) const {
    if(auto* guest=GuestFor(player)) return guest->hands;
    return hostHands;
}
bool cCoopNetwork::GroupItemType(int type) {
    return type==eGameItemType_Flashlight || type==eGameItemType_GlowStick || type==eGameItemType_WeaponMelee ||
        type==eGameItemType_Notebook || type==eGameItemType_Map;
}
void cCoopNetwork::ShareGroupItem(cInventory* source, cInventoryItem* item) {
    static bool sharing=false;
    if(!hosting || sharing || !item || !GroupItemType(item->GetItemType())) return;
    sharing=true;
    std::vector<std::pair<cPlayer*,cPlayerHands*>> players{{hostPlayer,hostHands}};
    for(auto& guest:guests) if(guest->player && guest->ownInventory) players.push_back({guest->player,guest->hands});
    for(auto& p:players) {
        cInventory* target=InventoryFor(p.first);
        if(target==source || target->GetItem(item->GetName())) continue;
        PlayerScope scope(init,p.first,p.second);
        mainInventory->SetSilent(true);
        target->AddItemFromFile(item->GetName(),item->GetEntityFile(),-1);
        mainInventory->SetSilent(false);
        Log("[COOP/INVENTORY] Group item %s given to player %s\n",item->GetName().c_str(),p.first==hostPlayer?"host":"guest");
    }
    sharing=false;
}
void cCoopNetwork::GiveGroupItems(cPlayer* player, cPlayerHands* hands) {
    if(!hosting || !player || !mainInventory) return;
    cInventory* target=InventoryFor(player);
    if(target==mainInventory) return;
    for(const auto& pair:mainInventory->NetworkItems()) {
        auto* item=pair.second;
        if(!GroupItemType(item->GetItemType()) || target->GetItem(item->GetName())) continue;
        PlayerScope scope(init,player,hands);
        mainInventory->SetSilent(true);
        target->AddItemFromFile(item->GetName(),item->GetEntityFile(),-1);
        mainInventory->SetSilent(false);
    }
}
bool cCoopNetwork::PartyHasItem(const std::string& name) const {
    if(!hosting) return false;
    if(mainInventory && mainInventory->GetItem(name)) return true;
    for(const auto& guest:guests) if(guest->ownInventory && guest->ownInventory->GetItem(name)) return true;
    return false;
}
bool cCoopNetwork::PartyRemoveItem(const std::string& name) {
    if(!hosting) return false;
    std::vector<std::pair<cPlayer*,cPlayerHands*>> players{{hostPlayer,hostHands}};
    for(auto& guest:guests) if(guest->player && guest->ownInventory) players.push_back({guest->player,guest->hands});
    for(auto& p:players) {
        cInventory* inventory=InventoryFor(p.first);
        if(auto* item=inventory->GetItem(name)) { PlayerScope scope(init,p.first,p.second); inventory->RemoveItem(item); return true; }
    }
    return false;
}
bool cCoopNetwork::TransferItem(const std::string& name, cPlayer* from, cPlayer* to) {
    if(!hosting || !from || !to || from==to) return false;
    cInventory* source=InventoryFor(from); cInventory* target=InventoryFor(to);
    auto* item=source->GetItem(name);
    if(!item || source==target) return false;
    const tString file=item->GetEntityFile(); const int count=item->GetCount(); const bool counted=item->HasCount();
    { PlayerScope scope(init,from,HandsFor(from)); source->RemoveItem(item); }
    {
        PlayerScope scope(init,to,HandsFor(to));
        mainInventory->SetSilent(true);
        target->AddItemFromFile(name,file,-1);
        mainInventory->SetSilent(false);
        if(counted) if(auto* added=target->GetItem(name)) added->SetCount(std::max(1,count));
    }
    Log("[COOP/INVENTORY] Item %s passed to player %s\n",name.c_str(),to==hostPlayer?"host":"guest");
    return target->GetItem(name)!=nullptr;
}
cPlayer* cCoopNetwork::ItemPartnerInFront(cPlayer* giver, float pickedDistance) const {
    if(!hosting || !giver || !giver->GetCamera()) return nullptr;
    const cVector3f eye=giver->GetCamera()->GetPosition(), forward=giver->GetCamera()->GetForward();
    cPlayer* best=nullptr; float bestAlong=std::min(2.0f,pickedDistance+0.5f);
    for(auto* p:TargetablePlayers()) {
        if(!p || p==giver || p->IsDead() || !p->GetCharacterBody()) continue;
        // Il corpo del compagno come un segmento verticale dai piedi alla testa.
        const cVector3f feet=p->GetCharacterBody()->GetFeetPosition();
        const float height=std::max(.5f,p->GetCharacterBody()->GetSize().y);
        const cVector3f center=feet+cVector3f(0,height*.5f,0), offset=center-eye;
        const float along=cMath::Vector3Dot(offset,forward);
        if(along<=.1f || along>bestAlong) continue;
        const cVector3f closest=eye+forward*along, side=closest-center;
        if(std::sqrt(side.x*side.x+side.z*side.z)>.45f || std::fabs(side.y)>height*.5f+.1f) continue;
        best=p; bestAlong=along;
    }
    return best;
}
void cCoopNetwork::PlayerHint(cPlayer* player, const std::string& text) {
    Log("[COOP/HINT] Hint to %s: %s\n",player==hostPlayer?"host":"guest",text.c_str());
    if(player==hostPlayer) { init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(text),3,false); return; }
    if(auto* guest=GuestFor(player)) {
        online::Presentation e; e.kind=online::PresentationKind::Hint; e.target=guest->session.GuestPlayerId(); e.text[0]=text; Present(e);
    }
}
bool cCoopNetwork::GiveItem(cPlayer* giver, cPlayer* to, cInventoryItem* item) {
    if(!hosting || !giver || !to || !item || giver==to) return false;
    cInventory* source=InventoryFor(giver); cInventory* target=InventoryFor(to);
    if(source==target || source->GetItem(item->GetName())!=item) return false;
    const tString name=item->GetName(), file=item->GetEntityFile();
    const std::string shown=AsciiText(item->GetGameName());
    const bool italian=ItalianGame(init);
    const std::string alreadyHas=italian?"Il compagno ha gia': ":"Your partner already has: ";
    // Torcia, glowstick, armi, taccuino e mappa: li ha già ogni giocatore.
    if(GroupItemType(item->GetItemType())) { PlayerHint(giver,alreadyHas+shown); return true; }
    // Le pile (batterie, antidolorifici, razzi...) si uniscono per tipo e
    // sottotipo, come in cInventory::AddItemFromFile; gli altri per nome.
    cInventoryItem* existing=nullptr;
    for(const auto& pair:target->NetworkItems()) {
        auto* other=pair.second;
        if(item->HasCount() ? (other->GetItemType()==item->GetItemType() && other->GetSubType()==item->GetSubType()) : other->GetName()==name) { existing=other; break; }
    }
    if(existing && !item->HasCount()) { PlayerHint(giver,alreadyHas+shown); return true; }
    // Una sola unità di una pila, altrimenti l'oggetto intero.
    const int units=item->HasCount() ? 1 : std::max(1,item->GetCount());
    if(giver->GetCurrentItem()==item && (!item->HasCount() || item->GetCount()<=1)) giver->SetCurrentItem(nullptr);
    {
        PlayerScope scope(init,giver,HandsFor(giver));
        if(item->HasCount() && item->GetCount()>1) item->AddCount(-1); else source->RemoveItem(item);
    }
    if(existing) existing->AddCount(units);
    else {
        PlayerScope scope(init,to,HandsFor(to));
        mainInventory->SetSilent(true);
        target->AddItemFromFile(name,file,-1);
        mainInventory->SetSilent(false);
        if(auto* added=target->GetItem(name)) { if(added->HasCount()) added->SetCount(units); }
    }
    const auto idOf=[&](cPlayer* p) { auto* guest=GuestFor(p); return guest ? unsigned(guest->session.GuestPlayerId()) : 1u; };
    Log("[COOP/INVENTORY] Item %s given by player %u to player %u units=%d\n",name.c_str(),idOf(giver),idOf(to),units);
    PlayerHint(giver,(italian?"Hai dato al compagno: ":"Given to your partner: ")+shown);
    PlayerHint(to,(italian?"Hai ricevuto: ":"Received: ")+shown);
    return true;
}
void cCoopNetwork::UseItemAction(cPlayer* player, const std::string& name) {
    auto* item=init->mpInventory->GetItem(name);
    auto* body=player->GetPickedBody();
    auto* entity=body?static_cast<iGameEntity*>(body->GetUserData()):nullptr;
    if(getenv("PENUMBRA_COOP_QA")) Log("[COOP/QA] Item action name=%s target=%s distance=%.3f maximum=%.3f\n",name.c_str(),entity?entity->GetName().c_str():"none",player->GetPickedDist(),entity?entity->GetMaxInteractDist():0);
    // Un compagno davanti, più vicino dell'entità mirata, riceve l'oggetto.
    if(cPlayer* partner=item ? ItemPartnerInFront(player,entity ? player->GetPickedDist() : 1e9f) : nullptr) { GiveItem(player,partner,item); return; }
    if(item && entity && entity->IsActive() && player->GetPickedDist()<=entity->GetMaxInteractDist())
        init->mpInventory->GetItemType(item->GetItemType())->OnUse(item,entity);
}
void cCoopNetwork::Reset() {
    // Nuova partita: anche gli inventari degli ospiti si svuotano (quello
    // dell'host è un modulo del gioco e viene azzerato dal motore).
    for(auto& guest:guests) { if(guest->ownInventory) guest->ownInventory->Reset(); guest->readNotes.clear(); }
}
bool cCoopNetwork::HostHasNote(const std::string& key) const {
    auto it=init->mpNotebook->GetNoteIterator();
    while(it.HasNext()) { auto* note=it.Next(); if(NoteKey(note->msTextCat,note->msTextEntry)==key) return true; }
    return false;
}
bool cCoopNetwork::NoteFound(cNotebook_Note* note) {
    if(!hosting || !note) return true;
    const std::string key=NoteKey(note->msTextCat,note->msTextEntry);
    Guest* finder=GuestFor(init->mpPlayer);
    // Agli altri giocatori in partita: un avviso. La nota è già nel diario di tutti.
    const std::string found=(ItalianGame(init)?"Il compagno ha trovato una nota: ":"Your partner found a note: ")+AsciiText(note->msName);
    if(finder) PlayerHint(hostPlayer,found);
    for(auto& guest:guests) if(guest.get()!=finder && guest->player && guest->session.Connected()) PlayerHint(guest->player,found);
    Log("[COOP/JOURNAL] Note %s found by player %u\n",key.c_str(),finder?unsigned(finder->session.GuestPlayerId()):unsigned(online::HostPlayerId));
    if(!finder) return true;
    // Si apre sul client di chi l'ha raccolta, appena il diario la contiene.
    online::Presentation e; e.kind=online::PresentationKind::Note; e.target=finder->session.GuestPlayerId(); e.text[0]=key; Present(e);
    return false;
}
void cCoopNetwork::NoteOpened(cNotebook_Note* note) {
    if(!note || !IsClient()) return;
    const std::string key=NoteKey(note->msTextCat,note->msTextEntry);
    if(key.size()<=128 && clientReadNotes.insert(key).second) ClientAction(online::PlayerAction::NoteRead,true,key);
}
void cCoopNetwork::SaveNotesRead(cSavedGame* save) const {
    save->mbCoopNotesRead=true;
    for(size_t n=0;n<3;++n) {
        tString& out=n==0?save->msNotesRead2:n==1?save->msNotesRead3:save->msNotesRead4; out.clear();
        if(n<guests.size()) for(const auto& key:guests[n]->readNotes) out+=key+"\n";
    }
}
void cCoopNetwork::LoadNotesRead(cSavedGame* save) {
    for(size_t n=0;n<guests.size() && n<3;++n) {
        auto& read=guests[n]->readNotes; read.clear();
        if(save->mbCoopNotesRead) {
            const tString& in=n==0?save->msNotesRead2:n==1?save->msNotesRead3:save->msNotesRead4;
            size_t start=0;
            for(size_t end=in.find('\n');end!=tString::npos;start=end+1,end=in.find('\n',start)) if(end>start && HostHasNote(in.substr(start,end-start))) read.insert(in.substr(start,end-start));
        } else {
            // Salvataggio precedente al diario personale: le note già trovate sono lette.
            auto it=init->mpNotebook->GetNoteIterator();
            while(it.HasNext()) { auto* note=it.Next(); read.insert(NoteKey(note->msTextCat,note->msTextEntry)); }
        }
    }
    if(!save->mbCoopNotesRead) { auto it=init->mpNotebook->GetNoteIterator(); while(it.HasNext()) it.Next()->mbRead=true; }
}
cInventory* cCoopNetwork::InventoryFor(const cPlayer* player) const {
    if(auto* guest=GuestFor(player)) if(guest->ownInventory) return guest->ownInventory;
    return mainInventory ? mainInventory : init->mpInventory;
}
cCoopNetwork::Guest* cCoopNetwork::GuestFor(const cPlayer* player) const {
    if(!hosting || !player) return nullptr;
    for(const auto& guest:guests) if(guest->player==player) return guest.get();
    return nullptr;
}
bool cCoopNetwork::Ready() const { return g->map.epoch && g->session.Connected() && (IsHost()?g->session.IsWorldApplied(g->map.epoch):positioned); }
// Stato per mappa di un ospite (host): tutto va rimandato dopo il cambio.
void cCoopNetwork::ResetGuestMap(Guest& guest) {
    guest.baselineSent=false; guest.published.clear(); guest.pendingPresentation.clear(); guest.pendingMedia.clear();
    guest.journalBytes.clear(); guest.mediaBytes.clear(); guest.inventoryBytes.clear();
    guest.buttons=0; guest.lastInput=0; guest.examineDown=false; guest.poseValid=false;
}
void cCoopNetwork::BeforeMapLoad() {
    if(IsHost()) { PrepareSave(); for(auto& guest:guests) guest->session.BeginMapLoad(); }
    for(auto& guest:guests) if(guest->player && guest->player->GetCharacterBody()) {
        PlayerScope scope(init,guest->player,guest->hands); guest->player->OnWorldExit(); guest->hands->OnWorldExit();
    }
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    if(world) {
        for(auto& pair:avatars) {
            auto& view=pair.second;
            for(auto* part:view.parts) if(part) world->DestroyMeshEntity(part);
            if(view.light) world->DestroyLight(view.light);
            if(view.tool) world->DestroyMeshEntity(view.tool);
            if(view.flashlight) world->DestroyMeshEntity(view.flashlight);
        }
        if(auto* physics=world->GetPhysicsWorld()) {
            for(auto& pair:enemyColliders) if(pair.second) physics->DestroyBody(pair.second);
        }
    }
    enemyColliders.clear(); avatars.clear();
    positioned=false; hasClientPosition=false; snapshotAge=0;
    prediction.Clear(); ResetPlayback();
    for(auto& guest:guests) { ResetGuestMap(*guest); guest->pendingSave.reset(); guest->pendingWeapon.clear(); }
    restoredEquipment=false; baselineNames.clear(); receivedJournal=0; pendingNote.clear(); deferredMessages.clear(); replicatedSounds.clear(); gReplicaMass.clear();
    currentMusic.clear(); currentSkybox.clear();
}
// Punto libero vicino a `center` per il corpo `body`: prima le posizioni
// storiche (a destra a 0,6 m, poi sinistra, dietro, davanti), poi anelli più
// larghi; mai dentro muri o arredi, né a meno di 0,5 m da un altro giocatore.
cVector3f cCoopNetwork::QAPlaceInFront(cPlayer* of, iCharacterBody* body) {
    // Un punto dentro un ostacolo fa rimbalzare il corpo: i colpi a terra
    // (OnHitGround) lo ferivano fino a terra e la prova dell'armadietto falliva.
    auto* physics=init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld();
    // Prima davanti a `of`, poi tutto intorno (davanti può esserci un muro):
    // chi chiama orienta poi lo sguardo verso il punto scelto.
    const float yaw=std::atan2(of->GetCamera()->GetForward().x,of->GetCamera()->GetForward().z);
    const cVector3f origin=of->GetCharacterBody()->GetFeetPosition();
    cVector3f feet=origin+cVector3f(std::sin(yaw),0,std::cos(yaw))*1.2f;
    bool found=false;
    for(float distance:{1.2f,1.5f,0.9f}) for(int step=0;step<12;++step) {
        if(found) break;
        const float angle=yaw+(step%2?1:-1)*((step+1)/2)*k2Pif/12;
        const cVector3f spot=origin+cVector3f(std::sin(angle),0,std::cos(angle))*distance;
        const auto matrix=cMath::MatrixTranslate(spot+cVector3f(0,.1f+body->GetSize().y/2,0));
        if(!physics->CheckShapeWorldCollision(nullptr,body->GetShape(),matrix,body->GetBody(),false,true,nullptr,false)) { feet=spot; found=true; }
    }
    body->SetForce(cVector3f(0)); body->SetForceVelocity(cVector3f(0)); body->SetMoveSpeed(eCharDir_Forward,0); body->SetMoveSpeed(eCharDir_Right,0);
    body->SetFeetPosition(feet);
    if(!found) Log("[COOP/QA] No free spot in front; using %.2f/%.2f/%.2f\n",feet.x,feet.y,feet.z);
    return feet;
}
cVector3f cCoopNetwork::FreeSpot(iCharacterBody* body,const cVector3f& center,const std::vector<cVector3f>& taken) {
    auto* physics=init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld();
    std::vector<cVector3f> candidates={cVector3f(0.6f,0,0),cVector3f(-0.6f,0,0),cVector3f(0,0,0.6f),cVector3f(0,0,-0.6f)};
    for(int trial=0;trial<24;++trial) {
        const float angle=trial*k2Pif/8, radius=.9f+.4f*(trial/8);
        candidates.push_back(cVector3f(std::cos(angle)*radius,0,std::sin(angle)*radius));
    }
    for(const auto& offset:candidates) {
        const auto feet=center+offset;
        bool crowded=false;
        for(const auto& other:taken) { auto d=other-feet; d.y=0; if(d.SqrLength()<0.25f) crowded=true; }
        if(crowded) continue;
        const auto matrix=cMath::MatrixTranslate(feet+cVector3f(0,.1f+body->GetSize().y/2,0));
        if(!physics->CheckShapeWorldCollision(nullptr,body->GetShape(),matrix,body->GetBody(),false,true,nullptr,false)) return feet;
    }
    Log("[COOP/MAP] No free spawn spot near the host; using the default offset\n");
    return center+cVector3f(0.6f,0,0);
}
// Toglie dal mondo il corpo di un ospite disconnesso, conservandone lo stato
// per il suo rientro nella stessa mappa (stessa identità).
void cCoopNetwork::ExitWorld(Guest& guest) {
    auto* player=guest.player;
    PlayerScope scope(init,player,guest.hands);
    guest.pendingSave=std::make_shared<cPlayer_GlobalSave>();
    player->SaveToGlobal(guest.pendingSave.get());
    guest.pendingSave->mState=ePlayerState_Normal; guest.pendingSave->mbActive=true;
    guest.pendingWeapon=guest.hands->GetCurrentModel(1)?guest.hands->GetCurrentModel(1)->msName:"";
    guest.pendingIdentity=guest.session.ExpectedIdentity();
    player->ChangeState(ePlayerState_Normal); player->SetPushBody(nullptr);
    player->OnWorldExit(); guest.hands->OnWorldExit();
    Log("[COOP/MAP] Player %u left; body removed from the world\n",unsigned(guest.session.GuestPlayerId()));
}
// Crea il corpo dell'ospite nel mondo, in un punto libero vicino a `center`.
void cCoopNetwork::EnterWorld(Guest& guest,const cVector3f& center,std::vector<cVector3f>& taken) {
    auto* player=guest.player;
    { PlayerScope scope(init,player,guest.hands); player->OnWorldLoad(); guest.hands->OnWorldLoad(); if(!guest.everInWorld) player->SetPower(100); }
    guest.everInWorld=true;
    // Players pass through each other (group 1) but, as in the original
    // game, still collide with enemies. Disabling character collision
    // altogether let players and enemies walk through each other.
    player->GetCharacterBody()->SetCharacterGroup(1);
    // Lo stato conservato all'uscita vale solo per lo stesso ospite.
    if(guest.pendingSave && guest.pendingIdentity && guest.pendingIdentity!=guest.session.ExpectedIdentity()) { guest.pendingSave.reset(); guest.pendingWeapon.clear(); }
    guest.pendingIdentity=0;
    if(guest.pendingSave) {
        // Stato e posizione dall'ultimo checkpoint caricato.
        PlayerScope scope(init,player,guest.hands);
        player->ChangeState(ePlayerState_Normal); player->LoadFromGlobal(guest.pendingSave.get()); RestoreWeapon(guest.pendingWeapon);
        guest.pendingSave.reset(); guest.pendingWeapon.clear();
        taken.push_back(player->GetCharacterBody()->GetFeetPosition());
        Log("[COOP/SAVE] Player %u joined with its saved state health=%.2f\n",unsigned(guest.session.GuestPlayerId()),player->GetHealth());
        return;
    }
    const cVector3f feet=FreeSpot(player->GetCharacterBody(),center,taken);
    player->GetCharacterBody()->SetFeetPosition(feet); taken.push_back(feet);
    player->GetCamera()->SetYaw(init->mpPlayer->GetCamera()->GetYaw());
    player->GetCamera()->SetPitch(init->mpPlayer->GetCamera()->GetPitch());
    player->GetCharacterBody()->SetYaw(player->GetCamera()->GetYaw());
}
void cCoopNetwork::AfterMapLoad(const tString& file) {
    if(!IsOnline()) return;
    if(IsHost()) {
        currentMap.file="maps/"+cString::GetFileName(file); currentMap.fingerprint=Fingerprint(currentMap.file);
        if(!currentMap.fingerprint) FatalError("Cannot fingerprint cooperative map '%s'\n",currentMap.file.c_str());
        spawn=init->mpPlayer->GetCharacterBody()->GetFeetPosition();
        init->mpPlayer->GetCharacterBody()->SetCharacterGroup(1);
        std::vector<cVector3f> taken={spawn};
        for(size_t n=0;n<guests.size();++n) {
            auto& guest=*guests[n];
            guest.map.file=currentMap.file; guest.map.fingerprint=currentMap.fingerprint; ++guest.map.epoch;
            // Il primo ospite entra sempre nel mondo, come nella partita a due;
            // gli altri solo se connessi, altrimenti quando si connettono: un
            // corpo di uno slot vuoto resterebbe, inattivo, vicino all'host.
            if(n==0 || guest.session.Connected()) EnterWorld(guest,spawn,taken);
            if(guest.session.Connected()) guest.session.SendMap(guest.map);
        }
    } else {
        init->mpPlayer->GetCharacterBody()->SetCharacterGroup(1);
        init->mpPlayer->SetActive(false); init->mpFadeHandler->FadeIn(0); g->session.SendMapReady(g->map.epoch);
    }
    Log("[COOP/MAP] Loaded epoch=%u map=%s role=%s\n",g->map.epoch,file.c_str(),IsHost()?"host":"client");
}
bool cCoopNetwork::ClientAction(online::PlayerAction action, bool pressed,const std::string& item,const std::string& second) {
    if(!IsClient()) return false;
    // Un clic (sinistro o destro) che chiude un messaggio del client resta sul
    // client: sull'host diventerebbe un'interazione (afferrare un oggetto) o
    // chiuderebbe il messaggio dell'host. Anche il rilascio non viene inviato.
    if((action==online::PlayerAction::Interact || action==online::PlayerAction::Attack) && item.empty()) {
        bool& consumed=action==online::PlayerAction::Interact?interactForMessage:examineForMessage;
        if(pressed && init->mpPlayer->GetState()==ePlayerState_Message) { consumed=true; init->mpGameMessageHandler->ShowNext(); return true; }
        if(!pressed && consumed) { consumed=false; return true; }
    }
    if(action==online::PlayerAction::Interact) { interactHeld=pressed; if(!pressed) gestureYaw=gesturePitch=0; }
    online::ActionRequest request; request.epoch=g->map.epoch; request.sequence=++actionSequence; request.action=action; request.pressed=pressed; request.item=item; request.secondItem=second;
    if(action==online::PlayerAction::Interact && pressed && init->mpPlayer->GetState()==ePlayerState_UseItem && init->mpPlayer->GetCurrentItem())
        request.item=init->mpPlayer->GetCurrentItem()->GetName();
    if(Ready()) g->session.SendAction(request);
    if(action==online::PlayerAction::Interact && pressed && !request.item.empty()) init->mpPlayer->ChangeState(ePlayerState_Normal);
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
    enemy->SelectNetworkTarget(TargetablePlayers(),dt);
    auto* target=enemy->GetNetworkTarget();
    if(!target) target=init->mpPlayer;
    PlayerScope scope(init,target,HandsFor(target));
    enemy->iGameEntity::OnUpdate(dt);
}
bool cCoopNetwork::EnemyTrigger(iGameEnemy* enemy,cGameTrigger* trigger) {
    auto* target=enemy->GetNetworkTarget(); if(!target) target=init->mpPlayer;
    PlayerScope scope(init,target,HandsFor(target));
    return enemy->HandleTrigger(trigger);
}
// Per ID meno uno: l'host e gli ospiti che hanno applicato il mondo (nullptr
// per gli altri). Su questi giocatori i nemici scelgono il bersaglio.
std::vector<cPlayer*> cCoopNetwork::TargetablePlayers() const {
    std::vector<cPlayer*> players(online::MaxPlayers,nullptr);
    players[0]=hostPlayer;
    for(const auto& guest:guests) {
        const uint8_t id=guest->session.GuestPlayerId();
        if(id>=2 && id<=online::MaxPlayers && guest->player && guest->player->GetCharacterBody() && guest->map.epoch &&
           guest->session.Connected() && guest->session.IsWorldApplied(guest->map.epoch)) players[id-1]=guest->player;
    }
    return players;
}
std::vector<cPlayer*> cCoopNetwork::DamageablePlayers() const {
    std::vector<cPlayer*> players;
    for(auto* p:TargetablePlayers()) if(p) players.push_back(p);
    return players;
}
cPlayer* cCoopNetwork::PlayerForBody(iPhysicsBody* body) const {
    auto* local=hostPlayer;
    if(local && local->GetCharacterBody() && local->GetCharacterBody()->GetBody()==body) return local;
    for(const auto& guest:guests) if(guest->player && guest->player->GetCharacterBody() && guest->player->GetCharacterBody()->GetBody()==body) return guest->player;
    // Enemy scopes may temporarily expose player 2 as init->mpPlayer.
    return nullptr;
}
void cCoopNetwork::DamagePlayer(cPlayer* p,float damage) {
    PlayerScope scope(init,p,HandsFor(p));
    p->Damage(damage,ePlayerDamageType_BloodSplash);
}
void cCoopNetwork::PrepareSave() {
    std::vector<cPlayer*> players={hostPlayer};
    for(const auto& guest:guests) players.push_back(guest->player);
    for(auto* player:players) {
        if(!player || !player->GetCharacterBody()) continue;
        PlayerScope scope(init,player,HandsFor(player));
        const auto state=player->GetState();
        if(state==ePlayerState_Grab || state==ePlayerState_Move || state==ePlayerState_Push) {
            player->ChangeState(ePlayerState_Normal); player->SetPushBody(nullptr);
        }
    }
}
void cCoopNetwork::SavePartner(cSavedGame* save) {
    save->mlCoopVersion=1;
    save->mbCoopInventories=true;
    SaveNotesRead(save);
    for(size_t n=0;n<guests.size() && n<3;++n) if(guests[n]->ownInventory)
        guests[n]->ownInventory->SaveToGlobal(n==0?&save->mInventory2:n==1?&save->mInventory3:&save->mInventory4);
    save->msHostWeapon=init->mpPlayerHands->GetCurrentModel(1)?init->mpPlayerHands->GetCurrentModel(1)->msName:"";
    PlayerScope scope(init,remotePlayer,remoteHands);
    remotePlayer->SaveToGlobal(&save->mPlayer2);
    save->mPlayer2.mState=ePlayerState_Normal; // A physics grab cannot survive a load.
    save->mPlayer2.mbActive=true;
    save->msClientWeapon=remoteHands->GetCurrentModel(1)?remoteHands->GetCurrentModel(1)->msName:"";
    // Ospiti 3 e 4, se il loro corpo è nel mondo.
    for(int n=1;n<3;++n) {
        bool& present=n==1?save->mbPlayer3:save->mbPlayer4;
        auto& data=n==1?save->mPlayer3:save->mPlayer4;
        auto& weapon=n==1?save->msPlayer3Weapon:save->msPlayer4Weapon;
        present=false; weapon.clear();
        if(size_t(n)>=guests.size() || !guests[n]->player || !guests[n]->player->GetCharacterBody()) continue;
        auto& guest=*guests[n];
        PlayerScope other(init,guest.player,guest.hands);
        guest.player->SaveToGlobal(&data);
        data.mState=ePlayerState_Normal; data.mbActive=true;
        weapon=guest.hands->GetCurrentModel(1)?guest.hands->GetCurrentModel(1)->msName:"";
        present=true;
    }
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
    // Inventari degli ospiti (l'host ha già caricato il proprio).
    for(size_t n=0;n<guests.size() && n<3;++n) {
        auto& guest=*guests[n];
        if(!guest.ownInventory || !guest.player) continue;
        PlayerScope scope(init,guest.player,guest.hands);
        // LoadFromGlobal aggiunge senza svuotare: senza Reset gli oggetti
        // di gruppo dati all'ingresso restano doppi.
        guest.ownInventory->Reset();
        if(save->mbCoopInventories) guest.ownInventory->LoadFromGlobal(n==0?&save->mInventory2:n==1?&save->mInventory3:&save->mInventory4);
        else { GiveGroupItems(guest.player,guest.hands); }
    }
    Log("[COOP/SAVE] Guest inventories restored=%d\n",save->mbCoopInventories?1:0);
    LoadNotesRead(save);
    Log("[COOP/SAVE] Guest read notes restored=%d first=%u\n",save->mbCoopNotesRead?1:0,guests.empty()?0u:unsigned(guests[0]->readNotes.size()));
    RestoreWeapon(save->msHostWeapon); Log("[COOP/SAVE] Host weapon restored state=%d\n",int(init->mpPlayer->GetState()));
    {
        PlayerScope scope(init,remotePlayer,remoteHands);
        remotePlayer->ChangeState(ePlayerState_Normal); remotePlayer->LoadFromGlobal(&save->mPlayer2); RestoreWeapon(save->msClientWeapon); Log("[COOP/SAVE] Client weapon restored state=%d\n",int(remotePlayer->GetState()));
    }
    Log("[COOP/SAVE] Restored host_health=%.2f client_health=%.2f host_power=%.2f client_power=%.2f host_pos=%s client_pos=%s host_weapon=%s client_weapon=%s\n",init->mpPlayer->GetHealth(),remotePlayer->GetHealth(),init->mpPlayer->GetPower(),remotePlayer->GetPower(),init->mpPlayer->GetCharacterBody()->GetPosition().ToString().c_str(),remotePlayer->GetCharacterBody()->GetPosition().ToString().c_str(),save->msHostWeapon.c_str(),save->msClientWeapon.c_str());
    for(int n=1;n<3 && size_t(n)<guests.size();++n) {
        auto& guest=*guests[n];
        guest.pendingSave.reset(); guest.pendingWeapon.clear();
        if(!(n==1?save->mbPlayer3:save->mbPlayer4)) continue;
        const auto& data=n==1?save->mPlayer3:save->mPlayer4;
        const auto& weapon=n==1?save->msPlayer3Weapon:save->msPlayer4Weapon;
        if(guest.player && guest.player->GetCharacterBody()) {
            PlayerScope other(init,guest.player,guest.hands);
            guest.player->ChangeState(ePlayerState_Normal); guest.player->LoadFromGlobal(const_cast<cPlayer_GlobalSave*>(&data)); RestoreWeapon(weapon);
            Log("[COOP/SAVE] Restored player=%u health=%.2f power=%.2f pos=%s\n",unsigned(guest.session.GuestPlayerId()),guest.player->GetHealth(),guest.player->GetPower(),guest.player->GetCharacterBody()->GetPosition().ToString().c_str());
        } else {
            guest.pendingSave=std::make_shared<cPlayer_GlobalSave>(data); guest.pendingWeapon=weapon; guest.pendingIdentity=0;
            Log("[COOP/SAVE] Player %u not in the world; checkpoint state kept until it joins\n",unsigned(guest.session.GuestPlayerId()));
        }
    }
    hostDeathTime=0;
    for(auto& guest:guests) {
        guest->deathTime=0; guest->lifeMask=0; guest->lastInput=0; guest->poseValid=false; guest->baselineSent=false;
        guest->published.clear(); guest->inventoryBytes.clear(); guest->journalBytes.clear(); guest->mediaBytes.clear();
    }
    g->latest.players[0]=Capture(init->mpPlayer,init->mpPlayerHands,online::HostPlayerId);
    g->latest.players[1]=Capture(remotePlayer,remoteHands,g->session.GuestPlayerId()); g->lastInput=0;
    g->baselineSent=false; g->published.clear(); g->inventoryBytes.clear(); g->journalBytes.clear(); g->mediaBytes.clear();
}
void cCoopNetwork::RequestSave() {
    if(IsClient()) ClientAction(online::PlayerAction::Checkpoint,true);
    else if(IsHost()) checkpointRequested=true;
}
// Tutti a terra da 5 s: ultimo checkpoint (o la mappa da capo). Altrimenti chi
// è a terra da 5 s rinasce accanto a un compagno vivo. Contano l'host e gli
// ospiti connessi che hanno applicato il mondo.
void cCoopNetwork::UpdateLife(float dt) {
    struct Life { cPlayer* player; cPlayerHands* hands; float* time; unsigned id; };
    std::vector<Life> players={{init->mpPlayer,init->mpPlayerHands,&hostDeathTime,online::HostPlayerId}};
    for(auto& guest:guests)
        if(guest->player && guest->player->GetCharacterBody() && guest->session.Connected() && guest->session.IsWorldApplied(guest->map.epoch))
            players.push_back({guest->player,guest->hands,&guest->deathTime,guest->session.GuestPlayerId()});
    bool allDown=true;
    for(auto& p:players) { *p.time=p.player->IsDead()?*p.time+dt:0; if(*p.time<5) allDown=false; }
    if(allDown) {
        if(init->mpSaveHandler->LoadCheckpoint()) return;
        Log("[COOP/LIFE] No valid checkpoint; restarting current map\n");
        const std::string file=cString::GetFileName(currentMap.file);
        init->mpMapHandler->Load(file,"link1");
        for(auto& p:players) { p.player->SetHealth(100); *p.time=0; }
        return;
    }
    for(auto& p:players) {
        if(*p.time<5) continue;
        const Life* buddy=nullptr;
        for(const auto& q:players) if(!q.player->IsDead()) { buddy=&q; break; }
        if(!buddy) continue;
        auto* body=p.player->GetCharacterBody();
        const auto near=buddy->player->GetCharacterBody()->GetFeetPosition();
        bool placed=false;
        for(int trial=0;trial<24 && !placed;++trial) {
            const float angle=trial*k2Pif/8, radius=.8f+.4f*(trial/8);
            const auto feet=near+cVector3f(std::cos(angle)*radius,.1f,std::sin(angle)*radius);
            auto matrix=cMath::MatrixTranslate(feet+cVector3f(0,body->GetSize().y/2,0));
            if(!init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld()->CheckShapeWorldCollision(nullptr,body->GetShape(),matrix,body->GetBody(),false,true,nullptr,false)) {
                PlayerScope scope(init,p.player,p.hands);
                body->SetForce(cVector3f(0)); body->SetForceVelocity(cVector3f(0)); body->SetMoveSpeed(eCharDir_Forward,0); body->SetMoveSpeed(eCharDir_Right,0); body->SetFeetPosition(feet); p.player->SetHealth(100); p.player->SetActive(true); p.player->ChangeState(ePlayerState_Normal);
                *p.time=0; placed=true; worldDirty=true;
                Log("[COOP/LIFE] Respawn player=%u near surviving partner\n",p.id);
            }
        }
    }
}
void cCoopNetwork::PublishJournal() {
    cNotebook_GlobalSave saved; init->mpNotebook->SaveToGlobal(&saved);
    online::JournalState next; next.epoch=g->map.epoch;
    auto notes=saved.mlstNotes.GetIterator();
    while(notes.HasNext()) { const auto& n=notes.Next(); online::JournalEntry e; e.kind=0; e.name=cString::To8Char(n.msName); e.group=n.msTextCat; e.textEntry=n.msTextEntry; e.read=g->readNotes.count(NoteKey(e.group,e.textEntry))>0; next.entries.push_back(e); }
    auto tasks=saved.mlstTasks.GetIterator();
    while(tasks.HasNext()) { const auto& t=tasks.Next(); online::JournalEntry e; e.kind=2; e.name=t.msName; e.textEntry=cString::To8Char(t.msText); next.entries.push_back(e); }
    const auto bytes=online::EncodeJournal(next);
    if(bytes.empty() && unsendableLogged.insert(std::to_string(g->map.epoch)+"/journal").second)
        Log("[NET/JOURNAL] Cannot encode journal entries=%u; the client cannot finish loading\n",unsigned(next.entries.size()));
    if(bytes!=g->journalBytes) { next.revision=g->journalRevision+1; if(g->session.SendJournal(next)) { g->journalBytes=bytes; g->journalRevision=next.revision; } }
}
void cCoopNetwork::StopSpectating(cPlayer* me) {
    spectating=false;
    if(me && me->GetCharacterBody()) {
        me->GetCharacterBody()->SetCamera(me->GetCamera());
        me->GetCamera()->SetPosition(me->GetCharacterBody()->GetFeetPosition()+cVector3f(0,me->GetSize().y+me->GetCameraHeightAdd(),0));
    }
    Log("[COOP/SPECTATOR] Stop role=%s\n",IsHost()?"host":"client");
}
void cCoopNetwork::UpdateSpectator(float dt) {
    cPlayer* me=IsHost()?hostPlayer:init->mpPlayer;
    const bool down=me && me->GetCharacterBody() && me->IsDead() && Ready();
    // Compagni vivi: occhi, direzione dello sguardo e ID.
    struct Target { uint8_t id; cVector3f eye; float yaw, pitch; };
    std::vector<Target> targets;
    if(down && IsHost()) {
        for(auto& guest:guests) {
            auto* p=guest->player;
            if(!p || !p->GetCharacterBody() || p->IsDead() || !guest->session.Connected() || !guest->session.IsWorldApplied(guest->map.epoch)) continue;
            targets.push_back({guest->session.GuestPlayerId(),p->GetCamera()->GetPosition(),p->GetCamera()->GetYaw(),p->GetCamera()->GetPitch()});
        }
    } else if(down) {
        const float eyeHeight=me->GetSize().y+me->GetCameraHeightAdd();
        for(uint8_t n=0;n<g->latest.playerCount && n<online::MaxPlayers;++n) {
            if(n==1) continue; // il giocatore di questo PC
            const auto& s=g->latest.players[n];
            if(!s.id || (s.flags&8) || !(s.flags&4)) continue;
            targets.push_back({s.id,cVector3f(s.x/1000.0f,s.y/1000.0f+eyeHeight,s.z/1000.0f),s.yaw/1000.0f,s.pitch/1000.0f});
        }
    }
    if(targets.empty()) { if(spectating) StopSpectating(me); return; }
    size_t index=0;
    for(size_t n=0;n<targets.size();++n) if(targets[n].id==spectatorTarget) index=n;
    // Clic (Interagisci): compagno successivo.
    if(spectating && targets.size()>1 && init->mpGame->GetInput()->BecameTriggerd("Interact")) {
        index=(index+1)%targets.size();
        Log("[COOP/SPECTATOR] Next target=%u\n",unsigned(targets[index].id));
    }
    const Target& t=targets[index];
    auto* camera=me->GetCamera();
    if(!spectating) {
        spectating=true; spectatorTime=0; spectatorFollowLogged=false;
        me->GetCharacterBody()->SetCamera(nullptr); // la telecamera non segue più il corpo a terra
        spectatorPos=camera->GetPosition(); spectatorYaw=camera->GetYaw(); spectatorPitch=camera->GetPitch();
        init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(ItalianGame(init)?"Sei a terra: guardi un compagno (clic: cambia compagno)":"You are down: watching a partner (click: next partner)"),4,false);
        Log("[COOP/SPECTATOR] Start role=%s target=%u targets=%u\n",IsHost()?"host":"client",unsigned(t.id),unsigned(targets.size()));
    }
    spectatorTarget=t.id; spectatorTime+=dt;
    // Inseguimento morbido: niente scatti né tremolii dagli aggiornamenti di rete.
    const float k=1.0f-std::exp(-dt*10.0f);
    spectatorPos+= (t.eye-spectatorPos)*k;
    float dYaw=t.yaw-spectatorYaw;
    while(dYaw>kPif) dYaw-=k2Pif;
    while(dYaw<-kPif) dYaw+=k2Pif;
    spectatorYaw+=dYaw*k; spectatorPitch+=(t.pitch-spectatorPitch)*k;
    camera->SetPosition(spectatorPos); camera->SetYaw(spectatorYaw); camera->SetPitch(spectatorPitch); camera->SetRoll(0);
    if(!spectatorFollowLogged && spectatorTime>1.5f) {
        spectatorFollowLogged=true;
        Log("[COOP/SPECTATOR] Following role=%s target=%u distance=%.3f\n",IsHost()?"host":"client",unsigned(t.id),(spectatorPos-t.eye).Length());
    }
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
    if(!IsHost() || !entity) return;
    // Ogni ospite che tiene, trascina o spinge l'oggetto lo lascia prima che si rompa.
    for(auto& guest:guests) {
        auto* player=guest->player;
        if(!player || !player->GetCharacterBody()) continue;
        const auto state=player->GetState();
        if(state!=ePlayerState_Grab && state!=ePlayerState_Move && state!=ePlayerState_Push) continue;
        for(int n=0;n<entity->GetBodyNum();++n) if(player->GetPushBody()==entity->GetBody(n)) {
            PlayerScope scope(init,player,guest->hands);
            if(getenv("PENUMBRA_COOP_QA")) Log("[COOP/QA] Release partner holding broken entity=%s state=%d\n",entity->GetName().c_str(),int(state));
            player->ChangeState(ePlayerState_Normal); player->SetPushBody(nullptr);
            break;
        }
    }
}
void cCoopNetwork::RelayParticle(const std::string& file,const cMatrixf& matrix) {
    if(!IsHost() || file.empty()) return;
    online::MediaState s; s.kind=online::MediaKind::Particle;
    s.name="CoopBreak_"+std::to_string(++particleSerial); s.id=EntityID(s.name); s.file=file; s.transform=Pack(matrix);
    s.flags=1; s.values[8]=s.values[9]=s.values[10]=1000;
    for(auto& guest:guests) if(guest->session.Connected() && guest->pendingMedia.size()<256) guest->pendingMedia.push_back(s);
    worldDirty=true;
}
bool cCoopNetwork::ClientLook(float yaw,float pitch) {
    if(!IsClient()) return false;
    lookYaw+=yaw; lookPitch+=pitch;
    if(interactHeld && init->mpPlayer->GetState()==ePlayerState_WeaponMelee) {
        auto direction=[](float v) { return v>.015f?1:v<-.015f?-1:0; };
        const int x=direction(yaw),y=direction(pitch);
        if((x && x!=gestureYaw) || (y && y!=gesturePitch)) {
            online::ActionRequest a; a.epoch=g->map.epoch; a.sequence=++actionSequence; a.action=online::PlayerAction::WeaponLook;
            a.lookYaw=int16_t(std::clamp(yaw*10000,-31420.0f,31420.0f)); a.lookPitch=int16_t(std::clamp(pitch*10000,-31420.0f,31420.0f));
            if(Ready()) g->session.SendAction(a);
            if(x) gestureYaw=x; if(y) gesturePitch=y;
        }
    }
    return interactHeld && (g->latest.players[1].flags&16);
}
bool cCoopNetwork::RelayMessage(const tWString& text) {
    if(!IsHost()) return false;
    auto* owner=GuestFor(init->mpPlayer);
    // Un messaggio personale dell'host resta sull'host.
    if(!owner && personalMessage) return false;
    online::Presentation e; e.kind=online::PresentationKind::Message; e.target=owner?owner->session.GuestPlayerId():0; e.text[0]=cString::To8Char(text); Present(e);
    return owner!=nullptr;
}
// Evento di presentazione per ogni ospite connesso interessato (target 0 = tutti).
void cCoopNetwork::Present(const online::Presentation& e) {
    for(auto& guest:guests)
        if(guest->session.Connected() && (!e.target || e.target==guest->session.GuestPlayerId()) && guest->pendingPresentation.size()<256)
            guest->pendingPresentation.push_back(e);
}
void cCoopNetwork::RelayFade(bool out,float seconds) {
    if(!IsHost()) return;
    online::Presentation e; e.kind=online::PresentationKind::Fade; e.values[0]=out?1:0; e.values[1]=int32_t(std::max(0.0f,seconds)*1000); Present(e);
}
void cCoopNetwork::RelayGuiSound(const std::string& file,float volume) {
    if(!IsHost()) return;
    auto* owner=GuestFor(init->mpPlayer);
    online::Presentation e; e.kind=online::PresentationKind::GuiSound; e.target=owner?owner->session.GuestPlayerId():0; e.text[0]=file; e.values[0]=int32_t(std::clamp(volume,0.0f,1.0f)*1000); Present(e);
}
void cCoopNetwork::Update(float dt) {
    // Finestra chiusa con la X o Alt+F4: stessa uscita ordinata del menu
    // (questo aggiornamento gira sempre, anche nei menu e in gioco singolo).
    if(hpl::gbQuitRequested) { hpl::gbQuitRequested=false; Log("[COOP] Window close requested; exiting\n"); init->mpGame->Exit(); }
    hpl::gbPhysicsSoundsMuted=IsClient(); // suoni della fisica: sul client arrivano dall'host
    if(!IsOnline()) return;
    steam.Update();
    if(IsHost()) transport.Poll(); else g->session.Poll();
    RunQA(dt);
    bool anyConnected=false;
    for(auto& entry:guests) {
        auto& guest=*entry;
        // Nuova connessione (o riconnessione) di questo ospite: un nuovo epoch
        // solo per lui, così gli altri ospiti non ricaricano la mappa.
        if(guest.session.Connected() && guest.generation!=guest.session.Generation()) {
            guest.generation=guest.session.Generation(); guest.baselineSent=false; guest.published.clear(); guest.poseValid=false;
            guest.inventoryBytes.clear(); guest.journalBytes.clear(); guest.mediaBytes.clear();
            positioned=false; restoredEquipment=false; ResetPlayback(); prediction.Clear();
            if(IsHost() && guest.map.epoch && guest.player && !guest.player->GetCharacterBody()) {
                std::vector<cVector3f> taken={hostPlayer->GetCharacterBody()->GetFeetPosition()};
                for(auto& other:guests) if(other->player && other->player->GetCharacterBody()) taken.push_back(other->player->GetCharacterBody()->GetFeetPosition());
                EnterWorld(guest,taken[0],taken);
                Log("[COOP/MAP] Player %u joined during the map\n",unsigned(guest.session.GuestPlayerId()));
            }
            if(IsHost() && guest.map.epoch) { ++guest.map.epoch; guest.session.SendMap(guest.map); }
            hadConnection=true;
        }
        if(guest.session.Connected()) { anyConnected=true; continue; }
        if(IsClient()) {
            init->mpPlayer->SetActive(false);
            if(hadConnection && !guest.session.AttemptActive() && guest.session.RetryAllowed()) guest.session.StartClient(guest.session.RemoteAddress(),guest.session.RemotePort(),true);
        }
        if(IsHost()) DestroyAvatar(guest.session.GuestPlayerId());
        // Ospiti 3-4: il corpo esce dal mondo (il primo resta, come nella
        // partita a due); posizione e stato tornano se rientra in questa mappa.
        if(IsHost() && &guest!=guests[0].get() && guest.player && guest.player->GetCharacterBody()) { ExitWorld(guest); continue; }
        if(guest.player && guest.player->GetCharacterBody()) { PlayerScope scope(init,guest.player,guest.hands); guest.player->SetActive(false); guest.player->GetCharacterBody()->SetMoveSpeed(eCharDir_Forward,0); guest.player->GetCharacterBody()->SetMoveSpeed(eCharDir_Right,0); }
    }
    if(!anyConnected) return;
    const auto perfStart=std::chrono::steady_clock::now();
    (IsHost()?g->diag.clock:clientDiag.clock).Tick(Milliseconds());
    InjectQAHitch();
    if(IsHost()) UpdateHost(dt); else UpdateClient(dt);
    UpdateSpectator(dt);
    ReportDiagnostics();
    const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-perfStart).count());
    perfSum+=elapsed; perfMax=std::max(perfMax,elapsed); ++perfSamples; perfTime+=dt;
    if(perfTime>=5) {
        Log("[COOP/PERF] role=%s average_us=%llu max_us=%llu samples=%u\n",IsHost()?"host":"client",(unsigned long long)(perfSum/perfSamples),(unsigned long long)perfMax,perfSamples);
        perfTime=0; perfSum=perfMax=0; perfSamples=0;
    }
}
void cCoopNetwork::UpdateHost(float dt) {
    ++hostTick;
    if(!currentMap.fingerprint) return;
    // Pose di inizio tick. The physics step that opened this tick integrated
    // the previous command, so each partner's pose is captured now and
    // acknowledges exactly that one. Applying this tick's command first would
    // send a pose that lacks it. Tutte le pose sono catturate prima di
    // simulare un ospite, così ogni snapshot riporta lo stesso istante.
    // Il contatore di teleport cresce quando un giocatore si sposta di oltre
    // 1 m in un tick (nessuna camminata o caduta lo fa).
    auto teleport=[](online::PlayerState& pose,cVector3f& sent,bool& hasSent,uint8_t& count) {
        const cVector3f feet(pose.x/1000.0f,pose.y/1000.0f,pose.z/1000.0f);
        if(hasSent && (feet-sent).SqrLength()>1.0f) ++count;
        sent=feet; hasSent=true; pose.teleport=count;
    };
    hostPose=Capture(init->mpPlayer,init->mpPlayerHands,online::HostPlayerId);
    teleport(hostPose,hostSentFeet,hostHasSentFeet,hostTeleportCount);
    for(auto& guest:guests) {
        guest->poseValid=false;
        if(!guest->session.Connected() || !guest->player || !guest->player->GetCharacterBody() || !guest->map.epoch) continue;
        guest->pose=Capture(guest->player,guest->hands,guest->session.GuestPlayerId()); guest->poseAck=guest->lastInput;
        teleport(guest->pose,guest->sentFeet,guest->hasSentFeet,guest->teleportCount);
        guest->poseValid=true;
    }
    dirtyWorld=worldDirty; worldDirty=false;
    for(auto& guest:guests) {
        if(!guest->poseValid) continue;
        Bind(*guest); UpdateGuest(dt);
    }
    Bind(*guests[0]);
    bool anyReady=false;
    for(auto& guest:guests) if(guest->map.epoch && guest->session.Connected() && guest->session.IsWorldApplied(guest->map.epoch)) anyReady=true;
    checkpointCooldown=std::max(0.0f,checkpointCooldown-dt);
    if(checkpointRequested && checkpointCooldown<=0 && anyReady) {
        checkpointRequested=false; checkpointCooldown=2;
        if(init->mpSaveHandler->SaveCheckpoint()) { online::Presentation e; e.kind=online::PresentationKind::Hint; e.text[0]="Checkpoint co-op salvato sull\x27host."; Present(e); init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(e.text[0]),3,false); }
    }
    if(anyReady) { UpdateLife(dt); UpdateLevelHints(dt); }
}
// Cabina (primo livello): lo script apre l'uscita solo quando il gruppo ha
// torcia, taccuino e glowstick, e non dice cosa manca. In co-op gli oggetti
// finiscono a giocatori diversi: se manca solo il taccuino da 30 s, lo si
// ricorda a tutti (poi ogni 90 s). La condizione dello script non cambia.
void cCoopNetwork::UpdateLevelHints(float dt) {
    auto* door=init->mpMapHandler->GetGameEntity("exitdoor",false);
    const bool cabin=init->mpMapHandler->GetCurrentMapName().find("level00_01_boat_cabin")!=std::string::npos;
    if(!cabin || !door || door->IsActive() || !PartyHasItem("flashlight") || !PartyHasItem("glowstick") || PartyHasItem("notebook")) { levelHintWait=0; return; }
    levelHintWait+=dt; if(levelHintWait<30) return;
    levelHintWait=-60;
    online::Presentation e; e.kind=online::PresentationKind::Hint; e.text[0]="To leave the cabin, someone still has to pick up the notebook.";
    Present(e); init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(e.text[0]),6,false);
    ++levelHints; Log("[COOP/HINT] Hint to all: %s\n",e.text[0].c_str());
}
// Simulazione e invio per l'ospite legato (g, remotePlayer, remoteHands).
void cCoopNetwork::UpdateGuest(float dt) {
    const bool canPlay=Ready() && init->mpGame->GetUpdater()->GetCurrentContainerName()=="Default";
    // Posa e conferma catturate in UpdateHost prima di applicare i comandi di
    // questo tick (vedi lì).
    const online::PlayerState remotePose=g->pose; const uint32_t remoteAck=g->poseAck;
    // One queued command per tick (protocol 4); `repeated` replays the previous
    // command while the jitter buffer refills.
    online::PlayerInput input; bool repeated=false;
    const bool hasInput=canPlay && !remotePlayer->IsDead() && g->session.NextInput(input,repeated) && input.epoch==g->map.epoch && !(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))!="hammer" && std::string(getenv("PENUMBRA_COOP_QA"))!="party" && ((qaTime>4 && qaTime<6) || (qaTime>15 && qaTime<18) || (qaTime>24 && qaTime<26)));
    if(canPlay && !remotePlayer->IsDead()) {
        // Diagnostics: how client commands map onto host ticks. The host keeps
        // only the newest input, so commands that arrive together are skipped.
        g->diag.queueDepth.Add(double(g->session.InputQueueDepth())); g->diag.queueTarget=unsigned(g->session.InputQueueTarget());
        if(!hasInput) ++g->diag.idleTicks;
        else if(!repeated) {
            const uint32_t advance=input.sequence-g->lastInput;
            if(g->lastInput && advance<1000) { g->diag.skippedInputs+=advance-1; g->diag.commandDrift+=int64_t(advance)-1; }
            const uint64_t now=Milliseconds();
            if(g->diag.lastArrival) g->diag.inputInterval.Add(double(now-g->diag.lastArrival));
            g->diag.lastArrival=now; ++g->diag.newInputs;
        } else { ++g->diag.reusedTicks; --g->diag.commandDrift; }
    }
    {
        PlayerScope scope(init,remotePlayer,remoteHands);
        remotePlayer->SetActive((init->mpPlayer==remotePlayer ? (hostPlayer->IsActive() || hostPlayer->IsDead()) : true) && !remotePlayer->IsDead());
        // Local UI and camera effects belong to their owner; remote physics has its own state.
        auto apply=[&](const online::PlayerInput& input,bool repeated) {
            if(remotePlayer->GetState()==ePlayerState_Move || remotePlayer->GetState()==ePlayerState_WeaponMelee) {
                if(!repeated && remotePlayer->GetState()==ePlayerState_Move) { remotePlayer->AddYaw(input.lookYaw/10000.0f); remotePlayer->AddPitch(input.lookPitch/10000.0f); }
                if(remotePlayer->GetState()==ePlayerState_WeaponMelee && g->simpleSwing) remotePlayer->AddYaw(0);
                if(remotePlayer->GetState()==ePlayerState_WeaponMelee && !qaAimLocked) { remotePlayer->GetCamera()->SetYaw(input.yaw/1000.0f); remotePlayer->GetCamera()->SetPitch(input.pitch/1000.0f); remotePlayer->GetCharacterBody()->SetYaw(input.yaw/1000.0f); }
            } else if(!qaAimLocked && !(getenv("PENUMBRA_COOP_QA") && qaStage==2 && qaTime<8.6f && remotePlayer->GetState()==ePlayerState_Normal)) {
                remotePlayer->GetCamera()->SetYaw(input.yaw/1000.0f); remotePlayer->GetCamera()->SetPitch(input.pitch/1000.0f); remotePlayer->GetCharacterBody()->SetYaw(input.yaw/1000.0f);
            }
            if((input.buttons&4) && !(g->buttons&4)) remotePlayer->StartRun();
            if(!(input.buttons&4) && (g->buttons&4)) remotePlayer->StopRun();
            // Il bit 2 è lo stato del client (accovacciato o no), non il tasto:
            // con "Accovacciati a interruttore" (predefinito) StartCrouch alterna
            // e StopCrouch non fa nulla, e l'ospite restava giù sull'host. Lo
            // stato si applica a ogni comando, così si riallinea da solo (anche
            // dopo un salto, durante il quale il gioco non cambia postura).
            {
                const bool crouch=(input.buttons&2)!=0; const auto move=remotePlayer->GetMoveState();
                if(crouch && move!=ePlayerMoveState_Crouch && move!=ePlayerMoveState_Jump) remotePlayer->ChangeMoveState(ePlayerMoveState_Crouch);
                if(!crouch && move==ePlayerMoveState_Crouch) remotePlayer->ChangeMoveState(ePlayerMoveState_Walk);
            }
            if((input.buttons&1) && !(g->buttons&1)) remotePlayer->Jump();
            remotePlayer->SetJumpButtonDown((input.buttons&1)!=0); g->buttons=input.buttons; g->lastInput=input.sequence;
            remotePlayer->MoveForwards(input.forward/1000.0f,dt); remotePlayer->MoveSideways(input.strafe/1000.0f,dt);
        };
        if(hasInput) {
            // After a client stall the backlog is caught up: the older command
            // is integrated now and the next one by the coming physics step, so
            // both are simulated and the snapshot acknowledges the newer one.
            online::PlayerInput extra;
            if(!repeated && g->session.CatchUpInput(extra)) {
                apply(input,false);
                if(extra.epoch==g->map.epoch) { remotePlayer->GetCharacterBody()->Update(dt); apply(extra,false); ++g->diag.caughtUp; }
            } else apply(input,repeated);
        } else { remotePlayer->StopInteract(); remotePlayer->MoveForwards(0,dt); remotePlayer->MoveSideways(0,dt); g->buttons=0; g->examineDown=false; }
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
    while(g->session.ReadAction(action)) {
        if(action.action==online::PlayerAction::NoteRead) {
            if(action.epoch==g->map.epoch && g->readNotes.size()<1024 && HostHasNote(action.item) && g->readNotes.insert(action.item).second)
                Log("[COOP/JOURNAL] Note %s read by player %u\n",action.item.c_str(),unsigned(g->session.GuestPlayerId()));
            continue;
        }
        if(action.epoch!=g->map.epoch || !canPlay || remotePlayer->IsDead()) continue;
        if(action.action==online::PlayerAction::Checkpoint) { checkpointRequested=true; continue; }
        PlayerScope scope(init,remotePlayer,remoteHands);
        if(action.action==online::PlayerAction::Interact) {
            if(action.pressed && !action.item.empty()) UseItemAction(remotePlayer,action.item);
            else if(action.pressed) {
                if(getenv("PENUMBRA_COOP_QA")) {
                    auto eye=remotePlayer->GetCamera()->GetPosition(), forward=remotePlayer->GetCamera()->GetForward();
                    Log("[COOP/QA] Native interact picked=%s distance=%.3f pitch=%.3f eye=%.3f/%.3f/%.3f forward=%.3f/%.3f/%.3f\n",remotePlayer->GetPickedBody()?remotePlayer->GetPickedBody()->GetName().c_str():"none",remotePlayer->GetPickedDist(),remotePlayer->GetCamera()->GetPitch(),eye.x,eye.y,eye.z,forward.x,forward.y,forward.z);
                }
                remotePlayer->StartInteract();
            } else remotePlayer->StopInteract();
        }
        if(action.action==online::PlayerAction::Attack) { g->examineDown=action.pressed; if(action.pressed) remotePlayer->StartExamine(); else remotePlayer->StopExamine(); }
        if(action.action==online::PlayerAction::Lantern && (!action.pressed || init->mpInventory->GetItem("flashlight"))) remotePlayer->GetFlashLight()->SetActive(action.pressed);
        if(action.action==online::PlayerAction::UseInventory) {
            if(!action.pressed) remotePlayer->StartHolster();
            else if(auto* item=init->mpInventory->GetItem(action.item)) {
                if(item->GetItemType()==eGameItemType_WeaponMelee) {
                    g->simpleSwing=action.secondItem=="simple";
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
    if(g->session.IsMapReady(g->map.epoch) && !g->baselineSent) {
        if(PublishWorld(true) && g->session.SendWorldReady(g->map.epoch)) g->baselineSent=true;
        else if((g->waitingWorld+=dt)>5) { g->waitingWorld=0; Log("[COOP/MAP] Baseline still pending epoch=%u published=%u\n",g->map.epoch,unsigned(g->published.size())); }
    }
    if(g->session.IsMapReady(g->map.epoch)) {
        PublishMedia();
        g->journalTime+=dt; if(g->journalTime>=.5f || g->journalBytes.empty()) { g->journalTime=0; PublishJournal(); }
        online::InventoryState next; next.epoch=g->map.epoch;
        std::set<std::string> sentNames; // il client rifiuta i nomi doppi
        for(const auto& pair:InventoryFor(remotePlayer)->NetworkItems()) {
            if(!sentNames.insert(pair.second->GetName()).second) continue;
            auto* item=pair.second; online::InventoryItem value; value.name=item->GetName(); value.image=item->GetEntityFile(); value.count=uint16_t(std::max(1,item->GetCount())); next.items.push_back(value);
        }
        auto bytes=online::EncodeInventory(next);
        if(bytes.empty() && unsendableLogged.insert(std::to_string(g->map.epoch)+"/inventory").second) {
            std::string names; for(const auto& v:next.items) names+=v.name+"("+v.image+") ";
            Log("[NET/INVENTORY] Cannot encode inventory items=%u: %s; the client cannot finish loading\n",unsigned(next.items.size()),names.c_str());
        }
        if(bytes!=g->inventoryBytes) {
            next.revision=g->inventory.revision+1;
            if(g->session.SendInventory(next)) { g->inventory=next; g->inventoryBytes=bytes; }
        }
        for(auto it=g->pendingPresentation.begin();it!=g->pendingPresentation.end();) { it->epoch=g->map.epoch; if(g->session.SendPresentation(*it)) it=g->pendingPresentation.erase(it); else break; }
        g->worldTime+=dt; if(dirtyWorld || g->worldTime+0.000001f>=1.0f/30) { g->worldTime=std::max(0.0f,g->worldTime-1.0f/30); PublishWorld(false); }
        else if(g->baselineSent) PublishHeld();
        g->sendTime+=dt; g->backupTime+=dt;
        if(g->sendTime+0.000001f>=1.0f/60) {
            g->sendTime=std::max(0.0f,g->sendTime-1.0f/60); g->latest.epoch=g->map.epoch; g->latest.sequence=++g->sequence; g->latest.acknowledgedInput=remoteAck; g->latest.hostTick=hostTick;
            // Host, destinatario, poi gli altri ospiti in partita.
            g->latest.players[0]=hostPose; g->latest.players[1]=remotePose; g->latest.playerCount=2;
            for(const auto& other:guests)
                if(other.get()!=g && other->poseValid && other->session.Connected() && other->session.IsWorldApplied(other->map.epoch) && g->latest.playerCount<online::MaxPlayers)
                    g->latest.players[g->latest.playerCount++]=other->pose;
            g->latest.players[1].flags=(g->latest.players[1].flags&~4)|((canPlay && !remotePlayer->IsDead() && ((g->latest.players[0].flags&4) || (g->latest.players[0].flags&8)))?4:0);
            // Un bit per ID: ogni morte o rinascita di un giocatore visibile.
            uint8_t deadMask=0;
            for(uint8_t n=0;n<g->latest.playerCount;++n) if(g->latest.players[n].flags&8) deadMask|=uint8_t(1u<<(g->latest.players[n].id-1));
            if(deadMask!=g->lifeMask) { g->session.SendLife(g->latest); g->lifeMask=deadMask; }
            g->session.SendSnapshot(g->latest);
            if(g->backupTime>=0.2f) { g->session.SendSnapshotBackup(g->latest); g->backupTime=0; }
        }
        UpdateAvatar(g->latest.players[1],dt);
    }
}
void cCoopNetwork::UpdateClient(float dt) {
    ++clientTick; JudgeSteps(false);
    if(hasHostTick) {
        const double delay=jitterDelay.Delay(); playback.Advance(newestHostTick,delay);
        clientDiag.playbackDelay.Add(double(newestHostTick)-playback.Tick()); if(playback.Tick()>=double(newestHostTick)) ++clientDiag.playbackHeld;
    }
    online::MapState received;
    if(g->session.ReadMap(received) && received.epoch!=g->map.epoch) {
        if(Fingerprint(received.file)!=received.fingerprint) { Log("[COOP/MAP] Data mismatch %s\n",received.file.c_str()); g->session.Stop(); return; }
        g->map=received;
        if(!init->mpMapHandler->Load(cString::GetFileName(g->map.file),"link1")) { g->session.Stop(); return; }
    }
    if(!g->map.epoch || !init->mpPlayer->GetCharacterBody()) return;
    online::EntityState entity; while(g->session.ReadEntity(entity)) if(entity.epoch==g->map.epoch) { if(!g->session.IsWorldApplied(g->map.epoch) && !(entity.flags&4)) baselineNames.insert(entity.name); ApplyEntity(entity); }
    online::MediaState media; while(g->session.ReadMedia(media)) if(media.epoch==g->map.epoch) ApplyMedia(media);
    online::Snapshot snapshot,life;
    bool hasSnapshot=g->session.ReadSnapshot(snapshot);
    while(g->session.ReadLife(life)) if(life.epoch==g->map.epoch && (!hasSnapshot || int32_t(life.sequence-snapshot.sequence)>0)) { snapshot=life; hasSnapshot=true; }
    if(hasSnapshot && snapshot.epoch==g->map.epoch) {
        g->latest=snapshot;
        if(snapshot.sequence!=lastSnapshot) {
            const uint64_t now=Milliseconds();
            if(lastSnapshotAt && std::string(getenv("PENUMBRA_COOP_QA")?getenv("PENUMBRA_COOP_QA"):"")=="hammer" && hammerTime>40 && hammerTime<48) { const float interval=(now-lastSnapshotAt)/1000.0f; intervalSum+=interval; intervalMax=std::max(intervalMax,interval); ++intervalCount; }
            lastSnapshotAt=now;
            if(clientDiag.lastArrival) clientDiag.snapshotInterval.Add(double(now-clientDiag.lastArrival));
            clientDiag.lastArrival=now; ++clientDiag.snapshots;
            partnerHistory.Add(snapshot.hostTick,snapshot.players[0]); selfHistory.Add(snapshot.hostTick,snapshot.players[1]);
            for(uint8_t n=2;n<snapshot.playerCount;++n) otherHistory[snapshot.players[n].id].Add(snapshot.hostTick,snapshot.players[n]); jitterDelay.Arrival(clientTick,snapshot.hostTick); NoteHostTick(snapshot.hostTick);
            const double hostStep=clientDiag.HostPose(snapshot.players[1].x/1000.0f,snapshot.players[1].z/1000.0f);
            if(hostStep>0 && hostStep<1 && hammerTime>39) qaHostSteps.push_back({clientTick,float(hostStep)});
            { const auto& h=snapshot.players[1]; const float f=h.forwardSpeed/1000.0f, r=h.sideSpeed/1000.0f; const float cap=(f>=0?h.forwardMax:h.backwardMax)/1000.0f;
              if(f||r) { const double nominal=std::min(std::sqrt(f*f+r*r),cap)/60; clientDiag.nominalStep.Add(nominal*100);
                  if(hostStep>=0 && hostStep<1) { ++clientDiag.movingPoses; if(hostStep<nominal*.2) ++clientDiag.stalledPoses; clientDiag.lastPoseFree=hostStep>=nominal*.8; } }
              else clientDiag.lastPoseFree=false; }
            snapshotAge=0;
            auto* player=init->mpPlayer; auto* body=player->GetCharacterBody(); const auto& s=snapshot.players[1];
            cVector3f target(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f);
            const bool teleported=hasReceivedTeleport && s.teleport!=receivedTeleport;
            receivedTeleport=s.teleport; hasReceivedTeleport=true;
            if(!positioned) {
                body->SetFeetPosition(target); body->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetPitch(s.pitch/1000.0f);
                positioned=true; Log("[COOP/STATE] First player baseline applied\n");
            } else if(teleported) {
                // Intentional teleport (respawn, script): snap, no correction.
                body->SetFeetPosition(target); prediction.Clear(); ++clientDiag.teleports; teleportTick=clientTick;
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
        // Partner on the host timeline, between the two poses around the
        // playback tick (no extrapolation past the newest pose).
        UpdateAvatar(Played(partnerHistory,snapshot.players[0]),dt);
        // Gli altri ospiti: chi non è più nello snapshot (uscito, non ancora
        // in partita) perde silhouette e cronologia.
        for(auto it=otherHistory.begin();it!=otherHistory.end();) {
            bool present=false;
            for(uint8_t n=2;n<snapshot.playerCount;++n) if(snapshot.players[n].id==it->first) { present=true; UpdateAvatar(Played(it->second,snapshot.players[n]),dt); }
            if(present) { ++it; continue; }
            DestroyAvatar(it->first); it=otherHistory.erase(it);
        }
        clientDiag.PartnerShown(avatars[online::HostPlayerId].position.x,avatars[online::HostPlayerId].position.z);
    }
    if(positioned) { const auto feet=init->mpPlayer->GetCharacterBody()->GetFeetPosition(); clientDiag.ShownPose(feet.x,feet.z); }
    snapshotAge+=dt;
    if(playback.Valid()) for(auto& pair:motion) {
        auto* e=init->mpMapHandler->GetGameEntity(pair.first,false); if(!e) continue;
        const Pose *from=nullptr,*to=nullptr; float alpha=0;
        const bool latestOnly=getenv("PENUMBRA_COOP_INTERPOLATION") && std::string(getenv("PENUMBRA_COOP_INTERPOLATION"))=="legacy";
        if(!pair.second.history.Sample(latestOnly?double(newestHostTick):playback.Tick(),from,to,alpha)) continue;
        if(to->meshMotion && from->meshMotion && e->GetMeshEntity()) e->GetMeshEntity()->SetMatrix(cMath::MatrixSlerp(alpha,from->mesh,to->mesh,true));
        const bool drive=BodyDriveEnabled() && e->GetType()!=eGameEntityType_Enemy;
        // L'host non sposta il corpo da un secondo: è fermo.
        const bool resting=double(pair.second.history.NewestTick())+60<playback.Tick();
        // Fermo e già al suo posto: niente SetMatrix a ogni fotogramma (ognuno
        // passa dalla fisica; con molti oggetti il costo cresceva nel tempo).
        const bool settle=!drive && resting && alpha>=1;
        if(settle && pair.second.settled && pair.second.settledTick==pair.second.history.NewestTick()) continue;
        pair.second.settled=settle; pair.second.settledTick=pair.second.history.NewestTick();
        auto& drives=pair.second.drive; if(drives.size()<to->bodies.size()) drives.resize(to->bodies.size());
        const cVector3f gravity=init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld()->GetGravity()*dt;
        for(size_t n=0;n<to->bodies.size() && n<from->bodies.size() && n<size_t(e->GetBodyNum());++n) {
            const auto target=cMath::MatrixSlerp(alpha,from->bodies[n],to->bodies[n],true);
            auto* body=e->GetBody(int(n));
            if(drive && !body->IsCharacter()) DriveBody(body,target,resting && alpha>=1,drives[n],dt,gravity); else body->SetMatrix(target);
        }
    }
    ApplyHeldPrediction();
    online::Presentation event;
    while(g->session.ReadPresentation(event)) {
        if(event.epoch!=g->map.epoch || (event.target && event.target!=g->session.GuestPlayerId())) continue;
        if(event.kind==online::PresentationKind::Hint) init->mpEffectHandler->GetSubTitle()->Add(cString::To16Char(event.text[0]),3,false);
        if(event.kind==online::PresentationKind::Note) { pendingNote=event.text[0]; pendingNoteTime=0; }
        if(event.kind==online::PresentationKind::Message) {
            // Un messaggio chiude il taccuino: chi sta leggendo lo vede dopo.
            if(init->mpNotebook->IsActive() && deferredMessages.size()<16) { deferredMessages.push_back(cString::To16Char(event.text[0])); Log("[COOP/JOURNAL] Message deferred while reading: %s\n",event.text[0].c_str()); }
            else init->mpGameMessageHandler->Add(cString::To16Char(event.text[0]));
        }
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
    if(g->session.ReadInventory(items) && items.epoch==g->map.epoch && (items.revision!=g->inventory.revision || items.epoch!=g->inventory.epoch)) {
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
        g->inventory=items; applyingBaseline=false;
    }
    // Confirm only after every baseline presentation queued in this poll was applied.
    online::JournalState book;
    // Il taccuino aperto non si ricostruisce sotto gli occhi di chi legge:
    // l'aggiornamento aspetta che si chiuda (il primo della mappa no).
    const bool reading=init->mpNotebook->IsActive() && receivedJournal;
    if(!reading && g->session.ReadJournal(book) && book.epoch==g->map.epoch && book.revision!=receivedJournal) {
        cNotebook_GlobalSave saved;
        for(const auto& e:book.entries) {
            if(e.kind==0) {
                const std::string key=NoteKey(e.group,e.textEntry);
                // Letta qui ma non ancora confermata: l'host la riceve di nuovo.
                if(e.read) clientReadNotes.erase(key);
                else if(clientReadNotes.count(key)) ClientAction(online::PlayerAction::NoteRead,true,key);
                cNotebookNote_GlobalSave n; n.msName=cString::To16Char(e.name); n.msTextCat=e.group; n.msTextEntry=e.textEntry; n.mbRead=e.read || clientReadNotes.count(key); saved.mlstNotes.Add(n);
            }
            if(e.kind==2) { cNotebookTask_GlobalSave t; t.msName=e.name; t.msText=cString::To16Char(e.textEntry); saved.mlstTasks.Add(t); }
        }
        if(init->mpNotebook->IsActive()) init->mpNotebook->SetActive(false);
        init->mpNotebook->Reset(); init->mpNotebook->LoadFromGlobal(&saved); receivedJournal=book.revision;
    }
    if(reading && getenv("PENUMBRA_COOP_QA") && g->session.ReadJournal(book) && book.epoch==g->map.epoch && book.revision!=receivedJournal) qaDiaryDeferred=true;
    // Messaggi arrivati durante la lettura: ora che il taccuino è chiuso.
    if(!deferredMessages.empty() && !init->mpNotebook->IsActive()) {
        for(const auto& text:deferredMessages) init->mpGameMessageHandler->Add(text);
        deferredMessages.clear();
    }
    // La nota raccolta da questo giocatore si apre appena è nel diario.
    if(!pendingNote.empty()) {
        cNotebook_Note* found=nullptr;
        auto notes=init->mpNotebook->GetNoteIterator();
        while(notes.HasNext() && !found) { auto* note=notes.Next(); if(NoteKey(note->msTextCat,note->msTextEntry)==pendingNote) found=note; }
        if(found && !init->mpNotebook->IsActive()) {
            init->mpNotebook->SetActive(true); init->mpNotebook->SetSelectedNote(found);
            init->mpNotebook->GetStateMachine()->ChangeState(eNotebookState_Note);
            Log("[COOP/JOURNAL] Picked-up note opened %s\n",pendingNote.c_str()); pendingNote.clear();
        } else if((pendingNoteTime+=dt)>5) { Log("[COOP/JOURNAL] Picked-up note %s not in the journal\n",pendingNote.c_str()); pendingNote.clear(); }
    }
    // Diagnostica: cosa manca ancora per confermare il mondo di questa mappa.
    if(!g->session.IsWorldApplied(g->map.epoch)) {
        g->waitingWorld+=dt;
        if(g->waitingWorld>5) {
            g->waitingWorld=0;
            Log("[COOP/MAP] Waiting to apply world epoch=%u positioned=%d inventory_epoch=%u journal=%u world_ready=%d\n",g->map.epoch,positioned?1:0,g->inventory.epoch,receivedJournal,g->session.IsWorldReady(g->map.epoch)?1:0);
        }
    } else g->waitingWorld=0;
    if(positioned && g->inventory.epoch==g->map.epoch && receivedJournal && !g->session.IsWorldApplied(g->map.epoch) && g->session.IsWorldReady(g->map.epoch)) {
        auto it=init->mpMapHandler->GetGameEntityIterator();
        while(it.HasNext()) { auto* e=it.Next(); if(!baselineNames.count(e->GetName())) e->SetActive(false); }
        applyingBaseline=true;
        const auto& s=g->latest.players[1];
        init->mpPlayer->GetFlashLight()->SetActive((s.flags&2)!=0);
        for(const auto& pair:init->mpInventory->NetworkItems()) if(pair.second->GetItemType()==eGameItemType_WeaponMelee) {
            const auto model=cString::ToLowerCase(pair.second->GetHudModelName());
            if((s.tinderboxes==1 && model=="hammer") || (s.tinderboxes==2 && model=="pickaxe")) RestoreWeapon(model);
        }
        applyingBaseline=false; restoredEquipment=true;
        g->session.SendWorldApplied(g->map.epoch);
    }
    if(Ready()) {
        online::PlayerInput input; input.epoch=g->map.epoch; input.sequence=++inputSequence;
        auto* p=init->mpPlayer; auto* keys=init->mpGame->GetInput();
        if(init->mpGame->GetUpdater()->GetCurrentContainerName()=="Default" && p->IsActive() && !init->mpGameMessageHandler->HasMessage()) {
            input.forward=(keys->IsTriggerd("Forward")?1000:0)-(keys->IsTriggerd("Backward")?1000:0);
            input.strafe=(keys->IsTriggerd("Right")?1000:0)-(keys->IsTriggerd("Left")?1000:0);
            input.buttons=(keys->IsTriggerd("Jump")?1:0)|(keys->IsTriggerd("Run")?4:0);
        }
        // La postura è uno stato: va inviata sempre, anche con un messaggio a schermo.
        if(p->GetMoveState()==ePlayerMoveState_Crouch) input.buttons|=2;
        if(getenv("PENUMBRA_COOP_QA")) { input.forward=input.strafe=0; input.buttons&=2; }
        if(qaPartyWalking) { input.forward=1000; p->MoveForwards(1,dt); }
        if(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))!="hammer" && qaTime>2 && qaTime<3) {
            input.forward=-1000; input.strafe=0; input.buttons=0; p->MoveForwards(-1,dt);
        }
        if(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))=="hammer" && hammerTime>52 && hammerTime<54.5f) { input.forward=1000; p->MoveForwards(1,dt); }
        if(getenv("PENUMBRA_COOP_QA") && std::string(getenv("PENUMBRA_COOP_QA"))=="hammer" && hammerTime>40 && hammerTime<48) {
            const int walk=getenv("PENUMBRA_COOP_QA_WALK")?std::clamp(atoi(getenv("PENUMBRA_COOP_QA_WALK")),100,1000):500; input.forward=int16_t((hammerTime<44)?walk:-walk); p->MoveForwards(input.forward/1000.0f,dt);
            if(hasClientPosition && uint32_t(clientTick-teleportTick)>2) { auto delta=p->GetCharacterBody()->GetFeetPosition()-lastClientPosition; delta.y=0; const float step=delta.Length(); qaClientSteps.push_back({clientTick,step});
                stepMax=std::max(stepMax,step); movementDistance+=step; }
            lastClientPosition=p->GetCharacterBody()->GetFeetPosition(); hasClientPosition=true;
        }
        input.yaw=int16_t(Wrapped(p->GetCamera()->GetYaw())*1000); input.pitch=int16_t(p->GetCamera()->GetPitch()*1000);
        input.lookYaw=int16_t(std::clamp(lookYaw*10000,-31420.0f,31420.0f)); input.lookPitch=int16_t(std::clamp(lookPitch*10000,-31420.0f,31420.0f)); lookYaw=lookPitch=0;
        g->session.SendInput(input); prediction.Push(input,dt);
    }
}
iPhysicsBody* cCoopNetwork::CreateCollider(const std::string& name,const cVector3f& size) {
    auto* world=init->mpGame->GetScene()->GetWorld3D(); auto* physics=world?world->GetPhysicsWorld():nullptr;
    if(!physics || size.y<=0) return nullptr;
    // Same shape as iCharacterBody: a sphere or an upright cylinder.
    const float radius=std::max(size.x,size.z)*0.5f; cMatrixf offset=cMath::MatrixRotateZ(kPi2f);
    iCollideShape* shape=std::fabs(radius*2-size.y)<0.01f ? physics->CreateSphereShape(radius,nullptr) : physics->CreateCylinderShape(radius,size.y,&offset);
    auto* body=physics->CreateBody(name,shape);
    // A character body without user data: the pick ray and light checks skip it,
    // character movement collides with it.
    body->SetMass(0); body->SetGravity(false); body->SetIsCharacter(true); body->SetCollideCharacter(true);
    return body;
}
// Client: the object this client holds is shown where the host keeps it
// relative to the client's camera, applied to the client's current camera, so
// it follows looking and walking immediately instead of one round trip later.
// Doors, levers and pushed objects (Move/Push) show their newest host pose
// without playback delay. The host stays authoritative for the physics.
void cCoopNetwork::ApplyHeldPrediction() {
    const auto& self=g->latest.players[1];
    // PENUMBRA_COOP_HELD_PREDICTION=0 keeps the plain playback, for comparisons.
    const bool disabled=getenv("PENUMBRA_COOP_HELD_PREDICTION") && std::string(getenv("PENUMBRA_COOP_HELD_PREDICTION"))=="0";
    iPhysicsBody* heldBody=nullptr; std::string heldName; int heldIndex=-1;
    if(self.heldMode && positioned && !disabled) {
        auto name=entityNames.find(self.heldEntity);
        auto history=name==entityNames.end()?motion.end():motion.find(name->second);
        auto* entity=name==entityNames.end()?nullptr:init->mpMapHandler->GetGameEntity(name->second,false);
        if(entity && history!=motion.end() && !history->second.history.Empty() && self.heldBody<entity->GetBodyNum()) {
            const Pose& newest=history->second.history.Newest(); heldBody=entity->GetBody(self.heldBody); heldName=name->second; heldIndex=self.heldBody;
            if(self.heldMode==uint8_t(online::HeldMode::Grab) && self.heldBody<newest.bodies.size()) {
                const online::PlayerState *from=nullptr,*to=nullptr; float alpha=0;
                if(selfHistory.Sample(double(history->second.history.NewestTick()),from,to,alpha)) {
                    auto camera=[](const cVector3f& eye,float yaw,float pitch) { auto m=cMath::MatrixMul(cMath::MatrixRotateY(yaw),cMath::MatrixRotateX(pitch)); m.SetTranslation(eye); return m; };
                    const auto& h=alpha<.5f?*from:*to;
                    const auto hostCamera=camera(cVector3f(h.x/1000.0f,(h.y+h.eyeHeight)/1000.0f,h.z/1000.0f),h.yaw/1000.0f,h.pitch/1000.0f);
                    auto* own=init->mpPlayer->GetCamera();
                    const auto clientCamera=camera(own->GetPosition(),own->GetYaw(),own->GetPitch());
                    const auto relative=cMath::MatrixMul(cMath::MatrixInverse(hostCamera),newest.bodies[self.heldBody]);
                    const auto target=cMath::MatrixMul(clientCamera,relative);
                    auto& drives=history->second.drive; if(drives.size()<=self.heldBody) drives.resize(self.heldBody+1);
                    if(BodyDriveEnabled()) DriveBody(heldBody,target,false,drives[self.heldBody],1.0f/60,init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld()->GetGravity()/60);
                    else heldBody->SetMatrix(target);
                }
            } else {
                auto& drives=history->second.drive; if(drives.size()<newest.bodies.size()) drives.resize(newest.bodies.size());
                for(size_t n=0;n<newest.bodies.size() && n<size_t(entity->GetBodyNum());++n) {
                    if(BodyDriveEnabled() && !entity->GetBody(int(n))->IsCharacter()) DriveBody(entity->GetBody(int(n)),newest.bodies[n],false,drives[n],1.0f/60,init->mpGame->GetScene()->GetWorld3D()->GetPhysicsWorld()->GetGravity()/60);
                    else entity->GetBody(int(n))->SetMatrix(newest.bodies[n]);
                }
            }
        }
    }
    // The held body never blocks its holder (on the host the PID keeps it in
    // front of the camera); restore its collision once released.
    if(heldName!=heldCollisionEntity || heldIndex!=heldCollisionIndex) {
        if(heldCollisionIndex>=0) if(auto* previous=init->mpMapHandler->GetGameEntity(heldCollisionEntity,false))
            if(heldCollisionIndex<previous->GetBodyNum()) previous->GetBody(heldCollisionIndex)->SetCollideCharacter(heldCollisionSaved);
        heldCollisionEntity=heldName; heldCollisionIndex=heldIndex;
        if(heldBody) { heldCollisionSaved=heldBody->GetCollideCharacter(); heldBody->SetCollideCharacter(false); }
    }
}
void cCoopNetwork::ReportDiagnostics() {
    const uint64_t now=Milliseconds();
    if(!diagReportAt) { diagReportAt=now; return; }
    if(now-diagReportAt<5000) return;
    const unsigned window=unsigned(now-diagReportAt); diagReportAt=now;
    char text[512];
    std::snprintf(text,sizeof text,"[COOP/NETDIAG] role=%s window_ms=%u rtt_ms=%u rtt_var_ms=%u",IsHost()?"host":"client",window,g->session.Ping(),g->session.PingVariance());
    std::string line=text;
    if(IsHost()) {
        auto& d=g->diag; d.totalCommandDrift+=d.commandDrift;
        const auto queue=g->session.TakeInputStats();
        std::snprintf(text,sizeof text," ticks=%u burst_ticks=%u new_input_ticks=%u reused_input_ticks=%u expired_input_ticks=%u skipped_inputs=%llu command_drift=%lld command_drift_total=%lld queue_target=%u queue_starvations=%u queue_dropped=%u queue_lost=%u queue_caught_up=%u",
            d.clock.ticks,d.clock.burst,d.newInputs,d.reusedTicks,d.idleTicks,(unsigned long long)d.skippedInputs,(long long)d.commandDrift,(long long)d.totalCommandDrift,d.queueTarget,queue.starvations,queue.dropped,queue.lost,queue.caughtUp);
        line+=text+d.clock.interval.Format("tick_gap_ms")+d.inputInterval.Format("new_input_gap_ms")+d.queueDepth.Format("queue_depth");
        d.ResetWindow();
    } else {
        auto& d=clientDiag;
        std::snprintf(text,sizeof text," ticks=%u burst_ticks=%u snapshots=%u unpredicted_ticks=%u replay_truncated=%u poses_over_2cm=%u poses_over_10cm=%u resets_over_25cm=%u hard_over_2m=%u",
            d.clock.ticks,d.clock.burst,d.snapshots,d.unpredictedTicks,d.replayTruncated,d.over2cm,d.over10cm,d.reset25cm,d.hard2m);
        std::snprintf(text+strlen(text),sizeof text-strlen(text)," host_path_m=%.3f shown_path_m=%.3f replay_steps=%u replay_blocked=%u moving_poses=%u stalled_poses=%u",d.hostPath,d.shownPath,d.replaySteps,d.replayBlocked,d.movingPoses,d.stalledPoses);
        std::snprintf(text+strlen(text),sizeof text-strlen(text)," playback_held=%u teleports=%u",d.playbackHeld,d.teleports);
        line+=text+d.playbackDelay.Format("playback_delay_ticks")+d.partnerJerk.Format("partner_jerk_cm",100)+d.hostStep.Format("host_step_cm",100)+d.nominalStep.Format("nominal_step_cm")+d.freeCorrection.Format("free_correction_cm",100)+d.clock.interval.Format("tick_gap_ms")+d.snapshotInterval.Format("snapshot_gap_ms")+d.correction.Format("correction_cm",100)+d.pending.Format("pending")+d.ackLag.Format("ack_lag");
        d.ResetWindow();
    }
    Log("%s\n",line.c_str());
}
// QA only: PENUMBRA_COOP_QA_HITCH=host|client stalls that role for 100 ms once per
// second during the scripted hammer walk, like a slow frame on a real PC. HPL1
// then runs the missed 60 Hz logic ticks back to back.
void cCoopNetwork::JudgeSteps(bool all) {
    while(!qaClientSteps.empty() && (all || uint32_t(clientTick-qaClientSteps.front().first)>20)) {
        const auto [tick,step]=qaClientSteps.front(); qaClientSteps.pop_front();
        float hostPeak=0; for(const auto& h:qaHostSteps) if(std::abs(int32_t(h.first-tick))<=20) hostPeak=std::max(hostPeak,h.second);
        const float excess=std::max(0.0f,step-hostPeak); stepExcessMax=std::max(stepExcessMax,excess);
        if(step>.1f) Log("[COOP/QA] Large client step=%.4f host_peak=%.4f excess=%.4f tick=%u\n",step,hostPeak,excess,tick);
    }
    while(!qaHostSteps.empty() && int32_t(clientTick-qaHostSteps.front().first)>60) qaHostSteps.pop_front();
}
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
        auto* entity=it.Next(); alive.insert(entity->GetName());
        if(!PublishEntity(entity,baseline)) complete=false;
    }
    for(auto it=g->published.begin();it!=g->published.end();) {
        if(alive.count(it->first)) { ++it; continue; }
        auto e=it->second; e.flags=4; e.sequence=++g->sequence; e.hostTick=hostTick;
        if(g->session.SendEntity(e)) it=g->published.erase(it); else { complete=false; ++it; }
    }
    return complete;
}
// Objects a player holds, moves or pushes are sent every tick instead of at the
// 30 Hz world rate, so doors, levers and carried objects respond sooner.
void cCoopNetwork::PublishHeld() {
    for(uint8_t n=0;n<g->latest.playerCount;++n) { const auto& player=g->latest.players[n];
        if(!player.heldMode) continue;
        auto it=init->mpMapHandler->GetGameEntityIterator();
        while(it.HasNext()) { auto* entity=it.Next(); if(EntityID(entity->GetName())==player.heldEntity) { PublishEntity(entity,false); break; } }
    }
}
bool cCoopNetwork::PublishEntity(iGameEntity* entity,bool baseline) {
    const auto& name=entity->GetName();
    {
        online::EntityState e; e.epoch=g->map.epoch; e.id=EntityID(name); e.name=name; e.file=entity->GetFileName();
        // Alcune mappe originali conservano percorsi del PC degli sviluppatori
        // o barre rovesciate: HPL risolve le risorse per nome, quindi basta il
        // nome del file (la validazione rifiuta i due punti e la barra rovesciata).
        std::replace(e.file.begin(),e.file.end(),'\\','/');
        if(e.file.find(':')!=std::string::npos) e.file=cString::GetFileName(e.file);
        e.flags=entity->IsActive()?1:0; e.subtype=uint16_t(entity->GetType());
        if(entity->GetType()==eGameEntityType_Enemy) {
            auto* enemy=static_cast<iGameEnemy*>(entity);
            e.health=int32_t(std::clamp(enemy->GetHealth(),0.0f,100000000.0f)*10);
            e.enemyState=uint8_t(std::max(0,enemy->GetCurrentStateId()));
            { auto* owner=GuestFor(enemy->GetNetworkTarget()); e.target=owner?owner->session.GuestPlayerId():online::HostPlayerId; }
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
        const auto found=g->published.find(name);
        const bool lifecycle=found==g->published.end() || found->second.flags!=e.flags || (e.subtype==eGameEntityType_Enemy && ((found->second.health<=0)!=(e.health<=0) || found->second.enemyState!=e.enemyState));
        const bool moved=found==g->published.end() || found->second.bodies!=e.bodies || found->second.transform!=e.transform || found->second.character!=e.character || !e.animations.empty();
        // Durante la baseline un'entità già inviata non va reinviata se non
        // cambia stato: i nemici animati risultano sempre "mossi" e venivano
        // rispediti a ogni tick sul canale affidabile, saturando la finestra
        // di 8 KiB; le ultime entità non partivano mai e la baseline restava
        // incompleta (level01_12_water_cave_tunnels). Il movimento riparte
        // come posa non affidabile dopo la conferma del mondo.
        if(baseline && found!=g->published.end() && !lifecycle) return true;
        if(lifecycle || moved) {
            e.sequence=++g->sequence; e.hostTick=hostTick;
            const bool sent=(baseline || lifecycle)?g->session.SendEntity(e):g->session.SendEntityMotion(e);
            if(!sent && online::EncodeEntity(e).empty() && unsendableLogged.insert(std::to_string(g->map.epoch)+"/"+name).second)
                Log("[NET/WORLD] Cannot encode entity name=%s file=%s type=%u bodies=%u animations=%u health=%d state=%u target=%u; the baseline cannot complete\n",
                    name.c_str(),e.file.c_str(),unsigned(e.type),unsigned(e.bodies.size()),unsigned(e.animations.size()),e.health,unsigned(e.enemyState),unsigned(e.target));
            if(sent) g->published[name]=e; else return false;
        }
    }
    return true;
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
    if(s.flags&4) { motion.erase(s.name); auto collider=enemyColliders.find(s.name); if(collider!=enemyColliders.end() && collider->second) collider->second->SetActive(false); return; }
    if(auto* mesh=entity->GetMeshEntity()) if(s.bodies.empty()) mesh->SetMatrix(Unpack(s.transform));
    if(entity->GetType()==eGameEntityType_Enemy) {
        auto* enemy=static_cast<iGameEnemy*>(entity); enemy->ApplyNetworkState(s.health/10.0f,s.enemyState);
        auto* body=enemy->GetMover()->GetCharBody(); const auto frame=Unpack(s.character);
        body->SetPosition(frame.GetTranslation());
        if(IsClient()) {
            auto& collider=enemyColliders[s.name];
            if(!collider) collider=CreateCollider("CoopEnemyBody_"+s.name,body->GetSize());
            if(collider) { collider->SetActive(s.health>0 && (s.flags&1) && !(s.flags&4)); collider->SetPosition(frame.GetTranslation()); }
        }
        if(auto* mesh=entity->GetMeshEntity()) {
            if(!s.liveMotion || s.health<=0) mesh->SetMatrix(Unpack(s.transform));
            for(int n=0;n<mesh->GetAnimationStateNum();++n) mesh->GetAnimationState(n)->SetActive(false);
            for(const auto& value:s.animations) if(value.index<mesh->GetAnimationStateNum()) {
                auto* a=mesh->GetAnimationState(value.index); a->SetActive(true); a->SetLoop(value.loop);
                a->SetWeight(value.weight/10000.0f); a->SetTimePosition(value.time/1000.0f);
            }
        }
    }
    entityNames[s.id]=s.name;
    Pose pose;
    if(s.liveMotion && s.health>0 && entity->GetType()==eGameEntityType_Enemy && entity->GetMeshEntity()) { pose.meshMotion=true; pose.mesh=Unpack(s.transform); }
    for(size_t n=0;n<s.bodies.size() && n<size_t(entity->GetBodyNum());++n) {
        auto* body=entity->GetBody(int(n)); pose.bodies.push_back(Unpack(s.bodies[n]));
        if(!body->IsCharacter() && BodyDriveEnabled() && entity->GetType()!=eGameEntityType_Enemy) {
            // Con la guida a velocità il corpo resta dinamico (massa e gravità
            // originali); una posa affidabile (baseline, cambio di stato) lo
            // blocca subito nella posa dell'host.
            if(!s.liveMotion) {
                auto& drives=motion[s.name].drive; if(drives.size()<=n) drives.resize(n+1);
                PinBody(body,Unpack(s.bodies[n]),drives[n]);
            }
            continue;
        }
        if(!body->IsCharacter()) {
            // On the host a character pushes bodies up to its push mass out of
            // the way (iCharacterBody push callback); the replica is static, so
            // such light bodies must not block the client's character either.
            if(body->GetMass()>0 && body->GetMass()<=init->mpPlayer->GetCharacterBody()->GetMaxPushMass()) body->SetCollideCharacter(false);
            if(body->GetMass()>0) gReplicaMass[body]=body->GetMass();
            body->SetMass(0); body->SetGravity(false); body->SetLinearVelocity(cVector3f(0)); body->SetAngularVelocity(cVector3f(0));
        }
        if(!s.liveMotion) body->SetMatrix(Unpack(s.bodies[n]));
    }
    // Live poses are played back on the host timeline; a reliable baseline or
    // lifecycle pose is applied at once and restarts the history from itself.
    auto& history=motion[s.name].history;
    if(!s.liveMotion) history.Clear();
    history.Add(s.hostTick,pose); NoteHostTick(s.hostTick);
}
// Posa di un altro giocatore sulla linea temporale dell'host, tra le due pose
// attorno al tick di riproduzione (nessuna estrapolazione oltre la più recente).
online::PlayerState cCoopNetwork::Played(const online::PoseHistory<online::PlayerState,32>& history,const online::PlayerState& newest) {
    online::PlayerState shown=newest;
    const online::PlayerState *from=nullptr,*to=nullptr; float alpha=0;
    const bool latestOnly=getenv("PENUMBRA_COOP_INTERPOLATION") && std::string(getenv("PENUMBRA_COOP_INTERPOLATION"))=="legacy";
    if(!latestOnly && playback.Valid() && history.Sample(playback.Tick(),from,to,alpha)) {
        shown=*to;
        if(from->teleport==to->teleport) {
            shown.x=int32_t(from->x+(to->x-from->x)*alpha); shown.y=int32_t(from->y+(to->y-from->y)*alpha); shown.z=int32_t(from->z+(to->z-from->z)*alpha);
            shown.yaw=int16_t(online::LerpAngle(from->yaw/1000.0f,to->yaw/1000.0f,alpha)*1000); shown.pitch=int16_t(from->pitch+(to->pitch-from->pitch)*alpha);
        }
    }
    return shown;
}
void cCoopNetwork::DestroyAvatar(uint8_t id) {
    auto found=avatars.find(id); if(found==avatars.end()) return;
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    auto& view=found->second;
    if(world) {
        for(auto* part:view.parts) if(part) world->DestroyMeshEntity(part);
        if(view.light) world->DestroyLight(view.light);
        if(view.tool) world->DestroyMeshEntity(view.tool);
        if(view.flashlight) world->DestroyMeshEntity(view.flashlight);
    }
    avatars.erase(found);
    Log("[COOP/AVATAR] Removed partner silhouette id=%u\n",unsigned(id));
}
void cCoopNetwork::UpdateAvatar(const online::PlayerState& s,float dt) {
    auto* world=init->mpGame->GetScene()->GetWorld3D(); if(!world) return;
    // Una silhouette per giocatore; i nomi del primo partner restano quelli storici.
    auto& view=avatars[s.id];
    const std::string suffix=s.id<=2?"":"_P"+std::to_string(s.id);
    if(!view.parts[0]) {
        // Procedural placeholder silhouette: no retail character model is distributed.
        const cVector3f sizes[]={cVector3f(.4f,.65f,.22f),cVector3f(.23f,.23f,.23f),cVector3f(.12f,.55f,.12f),cVector3f(.12f,.55f,.12f),cVector3f(.15f,.7f,.15f),cVector3f(.15f,.7f,.15f)};
        for(int n=0;n<6;++n) {
            std::string name="CoopAvatar_"+std::to_string(n)+suffix; auto* mesh=init->mpGame->GetGraphics()->GetMeshCreator()->CreateBox(name,sizes[n],"player_gui_hand.mat");
            view.parts[n]=world->CreateMeshEntity(name,mesh); view.parts[n]->SetIsSaved(false); view.parts[n]->SetCastsShadows(true);
        }
        view.light=world->CreateLightSpot("CoopPartnerFlashlight"+suffix,"player_flashlight_gobo.tga");
        view.light->SetIsSaved(false); view.light->SetDiffuseColor(cColor(1,1,.85f,1)); view.light->SetFarAttenuation(15); view.light->SetFOV(cMath::ToRad(50)); view.light->SetCastShadows(true);
        auto* flashlightMesh=init->mpGame->GetResources()->GetMeshManager()->CreateMesh("item_flashlight.dae");
        if(flashlightMesh) { view.flashlight=world->CreateMeshEntity("CoopPartnerFlashlightMesh"+suffix,flashlightMesh); view.flashlight->SetIsSaved(false); view.flashlight->SetCastsShadows(true); }
        Log("[COOP/AVATAR] Created partner silhouette\n");
    }
    cVector3f target(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f);
    // The client passes a pose interpolated on the host timeline and the host
    // its exact local one, so no extra smoothing (it only added lag).
    // PENUMBRA_COOP_INTERPOLATION=legacy restores the previous exponential
    // smoothing of the newest pose, for comparisons only.
    const bool legacy=IsClient() && getenv("PENUMBRA_COOP_INTERPOLATION") && std::string(getenv("PENUMBRA_COOP_INTERPOLATION"))=="legacy";
    if(legacy && view.hasPosition && (view.position-target).SqrLength()<=4) view.position+=(target-view.position)*std::min(1.0f,dt*20);
    else view.position=target;
    view.hasPosition=true;
    const float crouch=(s.flags&1)?0.7f:1;
    const cVector3f offsets[]={cVector3f(0,1.1f,0),cVector3f(0,1.58f,0),cVector3f(-.27f,1.1f,0),cVector3f(.27f,1.1f,0),cVector3f(-.11f,.35f,0),cVector3f(.11f,.35f,0)};
    for(int n=0;n<6;++n) { auto m=cMath::MatrixRotateY(s.yaw/1000.0f); auto offset=offsets[n]; offset.y*=crouch; m.SetTranslation(view.position+cMath::MatrixMul(m,offset)); view.parts[n]->SetMatrix(m); view.parts[n]->SetVisible(g->session.Connected() && !(s.flags&8)); }
    auto lightMatrix=cMath::MatrixMul(cMath::MatrixRotateY(s.yaw/1000.0f),cMath::MatrixRotateX(s.pitch/1000.0f)); lightMatrix.SetTranslation(view.position+cVector3f(0,s.eyeHeight/1000.0f,0));
    view.light->SetMatrix(lightMatrix); view.light->SetVisible((s.flags&2)!=0);
    if(view.flashlight) {
        auto m=cMath::MatrixMul(lightMatrix,cMath::MatrixRotateX(kPi2f));
        m.SetTranslation(lightMatrix.GetTranslation()+cMath::MatrixMul(lightMatrix.GetRotation(),cVector3f(-.23f,-.24f,-.3f)));
        view.flashlight->SetMatrix(m); view.flashlight->SetVisible(g->session.Connected() && (s.flags&2) && !(s.flags&8));
    }
    if(s.tinderboxes!=view.toolID) {
        if(view.tool) world->DestroyMeshEntity(view.tool);
        view.tool=nullptr; view.toolID=s.tinderboxes;
        if(view.toolID==1 || view.toolID==2) {
            auto* mesh=init->mpGame->GetResources()->GetMeshManager()->CreateMesh(view.toolID==1?"item_hammer.dae":"items_pickaxe.dae");
            if(mesh) { view.tool=world->CreateMeshEntity("CoopPartnerTool"+suffix,mesh); view.tool->SetIsSaved(false); view.tool->SetCastsShadows(true); }
        }
    }
    view.toolSwing+=( ((s.flags&64)?1.0f:0.0f)-view.toolSwing)*std::min(1.0f,dt*25);
    if(view.tool) {
        auto frame=cMath::MatrixMul(cMath::MatrixRotateY(s.yaw/1000.0f),cMath::MatrixRotateX(s.pitch/1000.0f));
        auto m=cMath::MatrixMul(frame,cMath::MatrixRotateX(-.3f-view.toolSwing*1.0f));
        m.SetTranslation(view.position+cVector3f(0,s.eyeHeight/1000.0f,0)+cMath::MatrixMul(frame,cVector3f(.23f,-.24f,-.3f)));
        view.tool->SetMatrix(m); view.tool->SetVisible(g->session.Connected() && !(s.flags&8));
    }
}
void cCoopNetwork::RunQA(float dt) {
    if(!getenv("PENUMBRA_COOP_QA") || (!Ready() && qaTime<=0)) return;
    qaTime+=dt;
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="maps") { RunMapsQA(dt); return; }
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="party") { RunPartyQA(dt); return; }
    if(std::string(getenv("PENUMBRA_COOP_QA"))=="cabin") { RunCabinQA(dt); return; }
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
            qaMoved=(remotePlayer->GetCharacterBody()->GetFeetPosition()-qaInitialPosition).SqrLength()>.01f && g->lastInput>30;
            Log("[COOP/QA] Remote movement distance=%.3f input=%u\n",(remotePlayer->GetCharacterBody()->GetFeetPosition()-qaInitialPosition).Length(),g->lastInput);
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
            // Inventari per giocatore: la chiave è dell'host, che la passa al
            // client col gesto del gioco (oggetto scelto, mira, clic).
            if(auto* key=mainInventory->GetItem("padlock_key")) {
                const cVector3f eye=init->mpPlayer->GetCamera()->GetPosition();
                const cVector3f feet=QAPlaceInFront(init->mpPlayer,remotePlayer->GetCharacterBody());
                const auto aim=AimAngles(eye,feet+cVector3f(0,remotePlayer->GetCharacterBody()->GetSize().y*.5f,0));
                init->mpPlayer->GetCamera()->SetYaw(aim.y); init->mpPlayer->GetCamera()->SetPitch(aim.x);
                init->mpPlayer->SetCurrentItem(key); init->mpPlayer->ChangeState(ePlayerState_UseItem); init->mpPlayer->StartInteract();
                Log("[COOP/QA] Key given by gesture=%d host_state=%d\n",InventoryFor(remotePlayer)->GetItem("padlock_key") && !mainInventory->GetItem("padlock_key")?1:0,int(init->mpPlayer->GetState()));
            }
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
            { std::string hostItems, guestItems; for(const auto& pair:mainInventory->NetworkItems()) hostItems+=pair.first+" ";
              for(const auto& pair:InventoryFor(remotePlayer)->NetworkItems()) guestItems+=pair.first+" ";
              Log("[COOP/QA] Inventories host=[%s] guest=[%s]\n",hostItems.c_str(),guestItems.c_str()); }
            // Passaggio dall'ospite all'host, con l'azione del client (stesso
            // fotogramma: nessun comando del client sposta la mira nel frattempo).
            {
                cInventory* guestInventory=InventoryFor(remotePlayer);
                { PlayerScope scope(init,remotePlayer,remoteHands); mainInventory->SetSilent(true);
                  guestInventory->AddItemFromFile("qa_painkillers","item_painkillers.ent",-1);
                  if(auto* pills=guestInventory->GetItem("qa_painkillers")) pills->SetCount(2);
                  guestInventory->AddItemFromFile("qa_flashlight","item_flashlight.ent",-1); // di gruppo: arriva anche all'host
                  mainInventory->SetSilent(false); }
                const cVector3f eye=init->mpPlayer->GetCamera()->GetPosition();
                const cVector3f feet=QAPlaceInFront(init->mpPlayer,remotePlayer->GetCharacterBody());
                const cVector3f guestEye=feet+cVector3f(0,remotePlayer->GetSize().y+remotePlayer->GetCameraHeightAdd(),0);
                const auto aim=AimAngles(guestEye,init->mpPlayer->GetCharacterBody()->GetFeetPosition()+cVector3f(0,init->mpPlayer->GetCharacterBody()->GetSize().y*.5f,0));
                remotePlayer->GetCamera()->SetPosition(guestEye); remotePlayer->GetCamera()->SetYaw(aim.y); remotePlayer->GetCamera()->SetPitch(aim.x);
                int hostPillsBefore=0; for(const auto& pair:mainInventory->NetworkItems()) if(pair.second->GetItemType()==eGameItemType_Painkillers) hostPillsBefore+=pair.second->GetCount();
                { PlayerScope scope(init,remotePlayer,remoteHands); UseItemAction(remotePlayer,"qa_painkillers"); UseItemAction(remotePlayer,"qa_flashlight"); }
                int hostPills=0; for(const auto& pair:mainInventory->NetworkItems()) if(pair.second->GetItemType()==eGameItemType_Painkillers) hostPills+=pair.second->GetCount();
                auto* guestPills=guestInventory->GetItem("qa_painkillers");
                const bool oneUnit=hostPills==hostPillsBefore+1 && guestPills && guestPills->GetCount()==1;
                const bool groupKept=guestInventory->GetItem("qa_flashlight")!=nullptr && mainInventory->GetItem("qa_flashlight")!=nullptr;
                // La torcia di prova non resta a nessuno dei due.
                { PlayerScope scope(init,remotePlayer,remoteHands); if(auto* light=guestInventory->GetItem("qa_flashlight")) guestInventory->RemoveItem(light); }
                if(auto* light=mainInventory->GetItem("qa_flashlight")) mainInventory->RemoveItem(light);
                Log("[COOP/QA] Guest gave one painkiller=%d (host %d->%d, guest left %d) flashlight kept=%d\n",oneUnit?1:0,hostPillsBefore,hostPills,guestPills?guestPills->GetCount():0,groupKept?1:0);
                qaPuzzle=qaPuzzle && oneUnit && groupKept;
            }
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
        if(full && qaStage==9 && g->map.file=="maps/level01_01_outside.dae" && Ready()) {
            qaTransition=true; qaDiaryAt=qaTime; Log("[COOP/QA] Native second-map barrier PASS epoch=%u\n",g->map.epoch); qaStage=10;
        }
        // Diario personale: l'ospite raccoglie una nota con la stessa funzione
        // della raccolta nel mondo (OnPickUp nel suo contesto).
        if(full && qaStage==10 && qaTime>qaDiaryAt+1) {
            auto* note=hplNew(cInventoryItem,(init)); note->SetName("qa_note"); note->InitFromFile("item_note.ent");
            bool kept=false;
            { PlayerScope scope(init,remotePlayer,remoteHands); kept=init->mpInventory->GetItemType(note->GetItemType())->OnPickUp(note,true); }
            if(!kept) hplDelete(note);
            qaDiaryHostClosed=!init->mpNotebook->IsActive() && HostHasNote(NoteKey("Notes","qa_note"));
            qaStage=11;
        }
        // Mentre l'ospite legge, il diario cambia: il suo taccuino resta aperto.
        if(full && qaStage==11 && qaTime>qaDiaryAt+3.5f) { init->mpNotebook->AddTask("qa_diary_task",_W("QA diary task")); qaStage=12; }
        if(full && qaStage==12 && qaTime>qaDiaryAt+7) {
            const std::string key=NoteKey("Notes","qa_note");
            bool hostUnread=false; auto it=init->mpNotebook->GetNoteIterator();
            while(it.HasNext()) { auto* n=it.Next(); if(NoteKey(n->msTextCat,n->msTextEntry)==key) hostUnread=!n->mbRead; }
            const bool guestRead=g->readNotes.count(key)>0;
            std::unique_ptr<cSavedGame> saved(new cSavedGame()); SaveNotesRead(saved.get()); g->readNotes.clear(); LoadNotesRead(saved.get());
            const bool restored=g->readNotes.count(key)>0;
            qaDiary=qaDiaryHostClosed && hostUnread && guestRead && restored;
            Log("[COOP/QA] Native diary host_notebook_closed=%d unread_for_host=%d read_by_guest=%d save_roundtrip=%d\n",qaDiaryHostClosed?1:0,hostUnread?1:0,guestRead?1:0,restored?1:0);
            qaStage=13;
        }
    } else {
        if(qaStage==0 && qaTime>5) {
            const auto& s=g->latest.players[1]; init->mpPlayer->GetCharacterBody()->SetFeetPosition(cVector3f(s.x/1000.0f,s.y/1000.0f,s.z/1000.0f));
            init->mpPlayer->GetCamera()->SetYaw(s.yaw/1000.0f); init->mpPlayer->GetCamera()->SetPitch(s.pitch/1000.0f); qaStage=1;
            auto it=init->mpMapHandler->GetGameEntityIterator(); while(it.HasNext()) { auto* e=it.Next(); if(e->GetName().find("whiskey")!=std::string::npos && e->GetBodyNum()) { qaObject=e->GetName(); qaObjectPosition=e->GetBody(0)->GetLocalPosition(); break; } }
        }
        // Come un giocatore: prima chiude i pensieri a schermo, poi interagisce.
        if(qaStage==1 && qaTime<8 && init->mpPlayer->GetState()==ePlayerState_Message) init->mpGameMessageHandler->ShowNext();
        if(full && qaStage==1 && qaTime>7.7f && !qaHandChecked) {
            auto* body=init->mpPlayer->GetPickedBody(); qaHandChecked=true;
            qaHand=body && body->GetMass()==0 && CoopPickMass(body)>0 && init->mpPlayer->GetCrossHairState()==eCrossHairState_Active;
            Log("[COOP/QA] Replica hand picked=%s mass=%.2f original=%.2f crosshair=%d\n",body?body->GetName().c_str():"none",body?body->GetMass():0.0f,CoopPickMass(body),int(init->mpPlayer->GetCrossHairState()));
        }
        if(qaStage==1 && qaTime>8) { ClientAction(online::PlayerAction::Interact,true); qaStage=2; }
        if(qaStage==2 && qaTime>9.5f && qaTime<10.5f) init->mpPlayer->GetCamera()->AddYaw(dt*.25f);
        if(auto* bottle=qaObject.empty()?nullptr:init->mpMapHandler->GetGameEntity(qaObject,false)) {
            if(qaStage==2 && qaTime<=9.5f) qaGrabStart=bottle->GetBody(0)->GetLocalMatrix();
            if(qaStage==2 && qaTime>9.5f && qaGrabResponse<0 && (bottle->GetBody(0)->GetLocalPosition()-qaGrabStart.GetTranslation()).Length()>.01f) {
                qaGrabResponse=(qaTime-9.5f)*1000; Log("[COOP/QA] Interaction response grab_ms=%.0f\n",qaGrabResponse);
            }
        }
        if(qaStage==2 && qaTime>11) { ClientAction(online::PlayerAction::Interact,false); qaStage=3; }
        if(qaStage==3 && qaTime>14) {
            auto* e=init->mpMapHandler->GetGameEntity(qaObject,false); const float distance=e?(e->GetBody(0)->GetLocalPosition()-qaObjectPosition).Length():0;
            qaReplicated=distance>.08f; Log("[COOP/QA] Replica grabbed object distance=%.3f\n",distance); qaStage=4;
        }
        if(full && qaStage==4 && qaTime>17) {
            auto& s=g->latest.players[1]; init->mpPlayer->GetCamera()->SetYaw(s.yaw/1000.0f); init->mpPlayer->GetCamera()->SetPitch(s.pitch/1000.0f); qaStage=5;
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
            auto& s=g->latest.players[1]; init->mpPlayer->GetCamera()->SetYaw(s.yaw/1000.0f); init->mpPlayer->GetCamera()->SetPitch(s.pitch/1000.0f); qaStage=8;
        }
        if(full && qaStage==8 && qaTime>27) { ClientAction(online::PlayerAction::Interact,true); qaStage=9; }
        if(full && qaStage==9 && qaTime>28 && qaTime<29.5f) init->mpPlayer->AddPitch(dt*2.0f);
        if(full && qaStage==9) if(auto* locker=init->mpMapHandler->GetGameEntity("locker1",false)) {
            if(qaTime<=28) qaDoorStart=locker->GetBody(1)->GetLocalMatrix();
            else if(qaDoorResponse<0) {
                const auto a=Pack(qaDoorStart), b=Pack(locker->GetBody(1)->GetLocalMatrix()); bool moved=false;
                for(size_t n=0;n<a.size();++n) if(std::abs(a[n]-b[n])>(n%4==3?10:100)) moved=true;
                if(moved) { qaDoorResponse=(qaTime-28)*1000; Log("[COOP/QA] Interaction response door_ms=%.0f\n",qaDoorResponse); }
            }
        }
        if(full && qaStage==9 && qaTime>30) { ClientAction(online::PlayerAction::Interact,false); qaStage=10; }
        if(full && qaStage==10 && qaTime>32) {
            auto* locker=init->mpMapHandler->GetGameEntity("locker1",false); auto a=Pack(qaDoorMatrix),b=Pack(locker->GetBody(1)->GetLocalMatrix());
            for(size_t n=0;n<a.size();++n) if(std::abs(a[n]-b[n])>100) qaDoorMoved=true;
            Log("[COOP/QA] Replica client door changed=%d\n",qaDoorMoved?1:0); qaStage=11;
        }
        if(full && qaStage==11 && g->map.file=="maps/level01_01_outside.dae" && Ready() && init->mpGame->GetGraphics()->GetRenderer3D()->GetFogActive() && init->mpGame->GetGraphics()->GetRenderer3D()->GetSkyBoxActive()) {
            qaTransition=true; qaDiaryAt=qaTime; Log("[COOP/QA] Replica second-map barrier PASS epoch=%u\n",g->map.epoch); qaStage=12;
        }
        if(full && qaStage==12) {
            auto* selected=init->mpNotebook->IsActive()?init->mpNotebook->GetSelectedNote():nullptr;
            if(!qaDiaryOpened && selected && NoteKey(selected->msTextCat,selected->msTextEntry)==NoteKey("Notes","qa_note")) { qaDiaryOpened=true; qaDiaryOpenedAt=qaTime; }
            if(qaDiaryOpened && qaTime>qaDiaryOpenedAt+4.5f) { qaDiaryHeld=init->mpNotebook->IsActive(); init->mpNotebook->SetActive(false); qaStage=13; }
            if(!qaDiaryOpened && qaTime>qaDiaryAt+9) { Log("[COOP/QA] Replica diary note never opened\n"); qaStage=14; }
        }
        if(full && qaStage==13 && qaTime>qaDiaryOpenedAt+6) {
            bool task=false, read=false;
            auto tasks=init->mpNotebook->GetTaskIterator(); while(tasks.HasNext()) if(tasks.Next()->msName=="qa_diary_task") task=true;
            auto notes=init->mpNotebook->GetNoteIterator(); while(notes.HasNext()) { auto* n=notes.Next(); if(NoteKey(n->msTextCat,n->msTextEntry)==NoteKey("Notes","qa_note")) read=n->mbRead; }
            qaDiary=qaDiaryOpened && qaDiaryHeld && qaDiaryDeferred && task && read;
            Log("[COOP/QA] Replica diary note_opened=%d stayed_open=%d update_deferred=%d task_after_close=%d read_kept=%d\n",qaDiaryOpened?1:0,qaDiaryHeld?1:0,qaDiaryDeferred?1:0,task?1:0,read?1:0);
            qaStage=14;
        }
        // Postura: il client si abbassa e si rialza; l'host deve seguirlo anche con
        // "Accovacciati a interruttore" (predefinito).
        if(full && qaStage==14 && qaCrouchStep==0) { init->mpPlayer->ChangeMoveState(ePlayerMoveState_Crouch); qaCrouchAt=qaTime; qaCrouchStep=1; }
        if(full && qaCrouchStep==1 && qaTime>qaCrouchAt+1.2f) { qaCrouchDown=(g->latest.players[1].flags&1)!=0; init->mpPlayer->ChangeMoveState(ePlayerMoveState_Walk); qaCrouchStep=2; }
        if(full && qaCrouchStep==2 && qaTime>qaCrouchAt+2.6f) {
            qaCrouchUp=!(g->latest.players[1].flags&1); qaCrouchStep=3;
            Log("[COOP/QA] Replica crouch host_followed_down=%d host_followed_up=%d toggle=%d\n",qaCrouchDown?1:0,qaCrouchUp?1:0,init->mpButtonHandler->GetToggleCrouch()?1:0);
        }
    }
    if(qaTime>(full?50:20)) {
        const bool ok=qaReplicated && (!IsHost() || (qaMoved && qaGrabbed)) && (!full || (qaPuzzle && qaTransition && qaDoorMoved && qaDiary && (!IsHost() || qaDoorGrabbed)))
            && (!full || IsHost() || (qaHand && qaCrouchDown && qaCrouchUp));
        Log("[COOP/QA] %s connected baseline, movement, native grab and replica object map=%s\n",ok?"PASS":"FAIL",g->map.file.c_str()); init->mpGame->Exit();
    }
}

// QA "maps": l'host carica una dopo l'altra le 25 mappe della campagna di
// Overture; su ognuna aspetta che il client abbia applicato il mondo
// (Ready), lascia replicare entità e media per 4 s e passa alla successiva.
// Passa se entrambi i ruoli completano tutte le mappe senza disconnessioni.
// Non verifica gli enigmi: solo caricamento, barriera e replica.
void cCoopNetwork::RunMapsQA(float dt) {
    static const char* maps[]={"level00_01_boat_cabin.dae","level01_01_outside.dae","level01_02_mine_entrance.dae","level01_03_shafts.dae",
        "level01_04_old_storage.dae","level01_05_office.dae","level01_06_workshop.dae","level01_07_lead_mine_shafts_A.dae",
        "level01_07_lead_mine_shafts_B.dae","level01_07_lead_mine_shafts_C.dae","level01_08_water_cave.dae","level01_09_generator_room.dae",
        "level01_10_small_shaft.dae","level01_11_new_storage_room.dae","level01_12_water_cave_tunnels.dae","level01_13_A_iron_mine_shafts.dae",
        "level01_13_B_iron_storage_tunnels.dae","level01_13_C_iron_base_tunnels.dae","level01_14_refinery.dae","level01_15_icecave.dae",
        "level01_16_destroyed_shaft.dae","level01_17_worm_nest.dae","level01_18_mining_room.dae","level01_19_door_shaft.dae","level01_20_base_entrance.dae"};
    const size_t count=sizeof(maps)/sizeof(maps[0]);
    const std::string current=cString::ToLowerCase(cString::GetFileName(g->map.file));
    if(Ready() && !qaMapsReady.count(current)) {
        qaMapsReady.insert(current); qaMapTime=0;
        int entities=0; auto it=init->mpMapHandler->GetGameEntityIterator(); while(it.HasNext()) { it.Next(); ++entities; }
        Log("[COOP/QA] Map ready role=%s index=%u map=%s epoch=%u entities=%d\n",IsHost()?"host":"client",unsigned(qaMapsReady.size()),current.c_str(),g->map.epoch,entities);
    }
    qaMapTime+=dt;
    if(IsHost() && Ready() && qaMapsReady.count(current) && qaMapTime>4 && qaMapIndex+1<count) {
        // Avanza alla mappa successiva nell'elenco (anche se lo script ne ha caricata un'altra).
        for(size_t n=0;n<count;++n) if(cString::ToLowerCase(maps[n])==current) qaMapIndex=n;
        if(qaMapIndex+1<count) { ++qaMapIndex; qaMapTime=0; init->mpMapHandler->ChangeMap(maps[qaMapIndex],"","","",.1f,.1f,"",""); }
    }
    size_t done=0; for(size_t n=0;n<count;++n) if(qaMapsReady.count(cString::ToLowerCase(maps[n]))) ++done;
    // L'uscita volontaria del partner dopo aver completato tutte le mappe non è un errore.
    if(!g->session.Connected() && !(done==count && g->session.PartnerLeftVoluntarily())) qaMapsDisconnected=true;
    const bool finished=done==count && qaMapTime>(IsHost()?8.0f:4.0f);
    if(finished || qaMapTime>90) {
        const bool ok=finished && !qaMapsDisconnected;
        Log("[COOP/QA] %s campaign maps role=%s ready=%u/%u disconnected=%d\n",ok?"PASS":"FAIL",IsHost()?"host":"client",unsigned(done),unsigned(count),qaMapsDisconnected?1:0);
        init->mpGame->Exit();
    }
}
// QA "party": l'host e 1-3 ospiti (PENUMBRA_COOP_PLAYERS giocatori in tutto).
// Quando tutti hanno applicato il mondo, ogni ospite cammina per 2,5 s.
// L'host verifica che ogni ospite abbia un ID distinto, che si sia mosso nella
// simulazione autoritativa e che il suo snapshot contenga tutti i giocatori.
// Ogni client verifica di ricevere tutti i giocatori e di essersi mosso.
void cCoopNetwork::RunPartyQA(float dt) {
    const char* countText=getenv("PENUMBRA_COOP_PLAYERS");
    const unsigned expected=countText?unsigned(std::clamp(atoi(countText),2,int(online::MaxPlayers))):2;
    if(IsHost()) {
        unsigned ready=0;
        for(auto& guest:guests) if(guest->session.Connected() && guest->map.epoch && guest->session.IsWorldApplied(guest->map.epoch)) ++ready;
        // Tutti pronti nella cabina: la prova si sposta all'aperto, dove
        // camminare dritto per qualche metro non urta l'arredo.
        const bool outside=currentMap.file=="maps/level01_01_outside.dae";
        if(!qaPartyStarted && ready+1==expected && !outside && !qaPartyMap) {
            qaPartyMap=true; init->mpMapHandler->ChangeMap("level01_01_outside.dae","link1","","",.1f,.1f,"",""); return;
        }
        if(!qaPartyStarted && ready+1==expected && outside) {
            qaPartyStarted=true; qaPartyTime=0;
            for(auto& guest:guests) if(guest->session.Connected()) qaPartyStart[guest->session.GuestPlayerId()]=guest->player->GetCharacterBody()->GetFeetPosition();
            qaPartyStart[online::HostPlayerId]=hostPlayer->GetCharacterBody()->GetFeetPosition();
            Log("[COOP/QA] Party ready role=host players=%u\n",expected);
        }
        if(qaPartyStarted) qaPartyTime+=dt;
        // Come nella prova hammer: l'host è attivo (gli ospiti seguono il suo
        // stato e i nemici ignorano i giocatori inattivi).
        if(qaPartyStarted) { hostPlayer->SetActive(true); init->mpGameMessageHandler->Reset(); }
        if(qaPartyStarted && qaPartyTime>10 && !qaPartyDone) {
            bool ok=true; std::set<uint8_t> ids;
            for(auto& guest:guests) {
                if(!guest->session.Connected()) continue;
                const uint8_t id=guest->session.GuestPlayerId(); ids.insert(id);
                auto delta=guest->player->GetCharacterBody()->GetFeetPosition()-qaPartyStart[id]; delta.y=0;
                const float moved=delta.Length();
                const bool guestOK=moved>0.5f && guest->latest.playerCount==expected;
                Log("[COOP/QA] Party guest id=%u moved_m=%.3f snapshot_players=%u ok=%d\n",unsigned(id),moved,unsigned(guest->latest.playerCount),guestOK?1:0);
                ok=ok && guestOK;
            }
            qaPartyMoved=ok && ids.size()+1==expected; qaPartyDone=true;
            // Fase del nemico: l'ultimo ospite resta solo, gli altri si
            // allontanano di 12 m, e un cane compare a 1,2 m da lui. Deve
            // sceglierlo come bersaglio (ID > 2 se i giocatori sono più di
            // due) e ferirlo con l'attacco nativo.
            for(auto& guest:guests) if(guest->session.Connected()) qaPartyVictim=guest.get();
            if(qaPartyVictim) {
                const cVector3f feet=qaPartyVictim->player->GetCharacterBody()->GetFeetPosition();
                // Gli altri tornano al punto di partenza (terreno noto), a circa 5 m dal cane.
                hostPlayer->GetCharacterBody()->SetFeetPosition(qaPartyStart[online::HostPlayerId]);
                for(auto& guest:guests) if(guest.get()!=qaPartyVictim && guest->player->GetCharacterBody()) guest->player->GetCharacterBody()->SetFeetPosition(qaPartyStart[guest->session.GuestPlayerId()]);
                qaPartyVictim->player->SetHealth(100);
                auto* world=init->mpGame->GetScene()->GetWorld3D();
                // Sul tratto appena percorso (c'è terreno), rivolto verso l'ospite.
                cVector3f back=qaPartyStart[qaPartyVictim->session.GuestPlayerId()]-feet; back.y=0;
                if(back.SqrLength()>0.01f) back.Normalise(); else back=cVector3f(0,0,-1);
                const cVector3f dogFeet=feet+back*1.2f+cVector3f(0,.2f,0);
                world->CreateEntity("CoopQA_PartyDog",cMath::MatrixTranslate(dogFeet+cVector3f(0,.3f,0)),"character_dog.ent",true);
                auto* dog=init->mpMapHandler->GetGameEntity("CoopQA_PartyDog",false);
                if(!dog || dog->GetType()!=eGameEntityType_Enemy) FatalError("[COOP/QA] Enemy fixture missing\n");
                if(world->GetAINodeList("ground")->empty()) for(int n=0;n<24;++n) world->AddAINode("CoopQA_PartyNode"+std::to_string(n),"ground",dogFeet+cVector3f((n%3-1)*.6f,.3f,(n/3)*.4f));
                auto* enemy=static_cast<iGameEnemy*>(dog);
                enemy->OnWorldLoad(); enemy->SetActive(true); enemy->GetMover()->GetCharBody()->SetFeetPosition(dogFeet); enemy->GetMover()->GetCharBody()->SetYaw(std::atan2(back.x,back.z));
                Log("[COOP/QA] Party enemy spawned near player=%u\n",unsigned(qaPartyVictim->session.GuestPlayerId()));
            }
            qaPartyTime=10;
        }
        if(qaPartyDone && qaPartyVictim) {
            auto* dog=static_cast<iGameEnemy*>(init->mpMapHandler->GetGameEntity("CoopQA_PartyDog",false));
            if(dog && dog->GetNetworkTarget()==qaPartyVictim->player) qaPartyTargeted=true;
            if(dog && qaPartyVictim->player->GetCharacterBody() && int(qaPartyTime*2)!=int((qaPartyTime-dt)*2)) {
                const auto p=dog->GetMover()->GetCharBody()->GetFeetPosition(), v=qaPartyVictim->player->GetCharacterBody()->GetFeetPosition();
                Log("[COOP/QA] Party dog t=%.1f active=%d state=%d health=%.0f target=%s pos=%.2f/%.2f/%.2f victim=%.2f/%.2f/%.2f victim_active=%d\n",qaPartyTime,dog->IsActive()?1:0,dog->GetCurrentStateId(),dog->GetHealth(),
                    !dog->GetNetworkTarget()?"none":dog->GetNetworkTarget()==hostPlayer?"host":"guest",p.x,p.y,p.z,v.x,v.y,v.z,qaPartyVictim->player->IsActive()?1:0);
            }
            if(qaPartyTargeted && qaPartyVictim->player->GetHealth()<99) qaPartyBitten=true;
            auto* victim=qaPartyVictim->player;
            if(qaPartyStage==0 && (qaPartyBitten || qaPartyTime>25)) {
                Log("[COOP/QA] Party enemy player=%u targeted=%d damaged=%d health=%.1f\n",unsigned(qaPartyVictim->session.GuestPlayerId()),qaPartyTargeted?1:0,qaPartyBitten?1:0,victim->GetHealth());
                if(!(qaPartyMoved && qaPartyTargeted && qaPartyBitten)) { Log("[COOP/QA] FAIL party of %u players role=host\n",expected); init->mpGame->Exit(); return; }
                // Fase del checkpoint: salute 50 salvata, poi cambiata e
                // ospite spostato; dopo il caricamento devono tornare quelle.
                if(dog) dog->SetActive(false);
                victim->SetHealth(50); qaPartySavedFeet=victim->GetCharacterBody()->GetFeetPosition();
                const bool saved=init->mpSaveHandler->SaveCheckpoint();
                Log("[COOP/QA] Party checkpoint saved=%d player=%u health=%.1f\n",saved?1:0,unsigned(qaPartyVictim->session.GuestPlayerId()),victim->GetHealth());
                victim->SetHealth(95); victim->GetCharacterBody()->SetFeetPosition(qaPartyStart[qaPartyVictim->session.GuestPlayerId()]);
                qaPartyStage=saved?1:9; qaPartyStageTime=qaPartyTime;
            }
            if(qaPartyStage==1 && qaPartyTime-qaPartyStageTime>1) {
                const bool loaded=init->mpSaveHandler->LoadCheckpoint();
                Log("[COOP/QA] Party checkpoint load started=%d\n",loaded?1:0);
                qaPartyStage=loaded?2:9; qaPartyStageTime=qaPartyTime; return;
            }
            if(qaPartyStage==2 && qaPartyTime-qaPartyStageTime>4 && ready+1==expected && victim->GetCharacterBody()) {
                auto delta=victim->GetCharacterBody()->GetFeetPosition()-qaPartySavedFeet; delta.y=0;
                const bool ok=std::fabs(victim->GetHealth()-50)<5 && delta.Length()<0.5f;
                Log("[COOP/QA] Party checkpoint restored player=%u health=%.1f position_error_m=%.3f ok=%d\n",unsigned(qaPartyVictim->session.GuestPlayerId()),victim->GetHealth(),delta.Length(),ok?1:0);
                if(!ok) { Log("[COOP/QA] FAIL party of %u players role=host\n",expected); init->mpGame->Exit(); return; }
                // Fase della riconnessione: l'host fa cadere la connessione
                // dell'ultimo ospite, che si ricollega da solo. Un ospite 3-4
                // deve uscire dal mondo e rientrare dov'era, con la sua salute.
                qaPartySavedFeet=victim->GetCharacterBody()->GetFeetPosition(); qaPartySavedHealth=victim->GetHealth();
                qaPartyVictim->session.RecoverConnection("QA: disconnessione forzata dell'ultimo ospite");
                qaPartyStage=3; qaPartyStageTime=qaPartyTime; return;
            }
            if(qaPartyStage==3) {
                const bool first=qaPartyVictim==guests[0].get();
                if(!qaPartyVictim->session.Connected() && !first && !victim->GetCharacterBody()) qaPartyBodyRemoved=true;
                if(qaPartyVictim->session.Connected() && qaPartyVictim->session.IsWorldApplied(qaPartyVictim->map.epoch) && victim->GetCharacterBody() && qaPartyTime-qaPartyStageTime>4) {
                    auto delta=victim->GetCharacterBody()->GetFeetPosition()-qaPartySavedFeet; delta.y=0;
                    const bool ok=(first || qaPartyBodyRemoved) && delta.Length()<0.5f && std::fabs(victim->GetHealth()-qaPartySavedHealth)<3;
                    Log("[COOP/QA] Party reconnect player=%u body_removed=%d position_error_m=%.3f health=%.1f/%.1f ok=%d\n",unsigned(qaPartyVictim->session.GuestPlayerId()),qaPartyBodyRemoved?1:0,delta.Length(),victim->GetHealth(),qaPartySavedHealth,ok?1:0);
                    Log("[COOP/QA] %s party of %u players role=host\n",ok?"PASS":"FAIL",expected);
                    init->mpGame->Exit(); return;
                }
            }
            if(qaPartyStage==9 || (qaPartyStage>=2 && qaPartyTime-qaPartyStageTime>60)) {
                Log("[COOP/QA] FAIL party of %u players role=host checkpoint_stage=%d\n",expected,qaPartyStage); init->mpGame->Exit(); return;
            }
        }
        if(!qaPartyStarted && qaTime>90) { Log("[COOP/QA] FAIL party of %u players role=host ready=%u\n",expected,ready+1); init->mpGame->Exit(); }
        return;
    }
    // Client: tutti in partita quando lo snapshot li contiene tutti.
    if(!qaPartyStarted && Ready() && g->latest.playerCount==expected && g->map.file=="maps/level01_01_outside.dae") {
        qaPartyStarted=true; qaPartyTime=0; qaPartyStart[0]=init->mpPlayer->GetCharacterBody()->GetFeetPosition();
        Log("[COOP/QA] Party ready role=client id=%u players=%u\n",unsigned(g->session.GuestPlayerId()),expected);
    }
    if(qaPartyStarted) qaPartyTime+=dt;
    qaPartyWalking=qaPartyStarted && qaPartyTime>3 && qaPartyTime<5.5f;
    // Dopo l'esito il client resta connesso: l'host controlla tutti insieme e
    // chiude per primo.
    // (una caduta breve, come quella forzata dall'host, è seguita dal rientro).
    if(qaPartyDone) { qaPartyOffline=g->session.Connected()?0:qaPartyOffline+dt; if(qaPartyOffline>12 || qaPartyTime>150) init->mpGame->Exit(); return; }
    if(qaPartyStarted && qaPartyTime>8) {
        qaPartyDone=true;
        auto delta=init->mpPlayer->GetCharacterBody()->GetFeetPosition()-qaPartyStart[0]; delta.y=0;
        const float moved=delta.Length();
        std::set<uint8_t> ids; for(uint8_t n=0;n<g->latest.playerCount;++n) ids.insert(g->latest.players[n].id);
        // Una silhouette per ogni altro giocatore (host compreso).
        const bool ok=moved>0.5f && g->latest.playerCount==expected && ids.size()==expected && g->latest.players[1].id==g->session.GuestPlayerId() && avatars.size()==expected-1;
        Log("[COOP/QA] %s party of %u players role=client id=%u moved_m=%.3f snapshot_players=%u avatars=%u\n",ok?"PASS":"FAIL",expected,unsigned(g->session.GuestPlayerId()),moved,unsigned(g->latest.playerCount),unsigned(avatars.size()));
        return;
    }
    if(!qaPartyStarted && qaTime>90) { Log("[COOP/QA] FAIL party of %u players role=client snapshot_players=%u\n",expected,unsigned(g->latest.playerCount)); init->mpGame->Exit(); }
}
void cCoopNetwork::RunCabinQA(float dt) {
    // Entrambi chiudono i pensieri con un clic, come un giocatore.
    const int phase=int(qaTime/2);
    if(qaTime>7 && phase!=qaCabinPhase && init->mpGameMessageHandler->HasMessage()) {
        if(IsHost()) init->mpPlayer->StartInteract(); else ClientAction(online::PlayerAction::Interact,true);
        qaCabinPhase=phase;
    }
    const bool hostExits=getenv("PENUMBRA_COOP_QA_EXIT") && std::string(getenv("PENUMBRA_COOP_QA_EXIT"))=="host";
    const bool outside=g->map.file=="maps/level01_01_outside.dae";
    auto items=[&](cInventory* inv) {
        std::string s; for(const char* n:{"flashlight","notebook","glowstick","battery1"}) if(inv && inv->GetItem(n)) { s+=n; s+=' '; }
        return s;
    };
    if(IsClient()) {
        if(outside && Ready()) {
            if(qaCabinAt<=0) qaCabinAt=qaTime;
            if(qaCabinStep==0 && qaTime>qaCabinAt+3) {
                const bool ok=init->mpInventory->GetItem("flashlight") && init->mpInventory->GetItem("notebook") && init->mpInventory->GetItem("glowstick");
                Log("[COOP/QA] %s cabin exit role=client map=%s items=[%s]\n",ok?"PASS":"FAIL",g->map.file.c_str(),items(init->mpInventory).c_str());
                qaCabinStep=1;
            }
        }
        if(qaTime>150) { Log("[COOP/QA] FAIL cabin exit role=client timeout map=%s\n",g->map.file.c_str()); init->mpGame->Exit(); }
        if(qaCabinStep==1 && qaTime>qaCabinAt+8) init->mpGame->Exit();
        return;
    }
    // Host: il personaggio `p` si mette vicino all'oggetto e lo guarda; il
    // raggio di scelta del gioco (stato normale) lo trova al tick successivo.
    auto place=[&](cPlayer* p,iGameEntity* e,float distance) {
        const cVector3f target=e->GetBody(0)->GetBV()->GetWorldCenter();
        cVector3f feet=p->GetCharacterBody()->GetFeetPosition(); cVector3f dir=feet-target; dir.y=0;
        if(dir.SqrLength()<1e-4f) dir=cVector3f(0,0,1);
        dir.Normalise(); feet=cVector3f(target.x,feet.y,target.z)+dir*distance;
        p->GetCharacterBody()->SetFeetPosition(feet);
        p->GetCharacterBody()->SetMoveSpeed(eCharDir_Forward,0); p->GetCharacterBody()->SetMoveSpeed(eCharDir_Right,0);
    };
    auto aim=[&](cPlayer* p,iGameEntity* e) {
        const auto a=AimAngles(p->GetCamera()->GetPosition(),e->GetBody(0)->GetBV()->GetWorldCenter());
        p->GetCamera()->SetYaw(a.y); p->GetCamera()->SetPitch(a.x); p->GetCharacterBody()->SetYaw(a.y);
    };
    auto take=[&](cPlayer* p,cPlayerHands* h,const char* name) {
        // Oggetti chiusi in cassetti o armadietti: raccolta diretta.
        auto* e=init->mpMapHandler->GetGameEntity(name,false); if(!e || !e->IsActive()) return false;
        PlayerScope scope(init,p,h); place(p,e,0.7f);
        p->SetPickedBody(e->GetBody(0)); p->GetPickRay()->mvPickedPos=e->GetBody(0)->GetBV()->GetWorldCenter(); e->PlayerInteract();
        return true;
    };
    auto* hostInv=mainInventory; auto* guestInv=InventoryFor(remotePlayer);
    const char* who=hostExits?"host":"guest";
    if(qaTime<1) { init->mpEffectHandler->GetDepthOfField()->SetActive(false,0); }
    if(qaCabinStep==0 && qaTime>9 && init->mpPlayer->IsActive() && remotePlayer->IsActive() && !init->mpGameMessageHandler->HasMessage()) {
        Log("[COOP/QA] Cabin unlocked host_active=1 guest_active=1 t=%.1f\n",qaTime); qaCabinStep=1; qaCabinAt=qaTime;
    }
    if(qaCabinStep==0 && qaTime>45) { Log("[COOP/QA] FAIL cabin intro never unlocked host_active=%d guest_active=%d message=%d\n",init->mpPlayer->IsActive()?1:0,remotePlayer->IsActive()?1:0,init->mpGameMessageHandler->HasMessage()?1:0); qaCabinStep=99; }
    // 1: l'ospite raccoglie il taccuino sul tavolo, col raggio di scelta del gioco.
    const bool late=getenv("PENUMBRA_COOP_QA_CABIN_LATE") && std::string(getenv("PENUMBRA_COOP_QA_CABIN_LATE"))=="1";
    if(qaCabinStep==1 && late) { qaCabinStep=2; qaCabinAt=qaTime; }
    if(qaCabinStep==1) {
        auto* book=init->mpMapHandler->GetGameEntity("notebook",false);
        if(book) { PlayerScope scope(init,remotePlayer,remoteHands); qaAimLocked=true; if(qaTime<qaCabinAt+0.2f) place(remotePlayer,book,0.8f); aim(remotePlayer,book); }
        if(qaTime>qaCabinAt+1.2f) {
            const bool picked=remotePlayer->GetPickedBody() && book && remotePlayer->GetPickedBody()==book->GetBody(0);
            { PlayerScope scope(init,remotePlayer,remoteHands); remotePlayer->StartInteract(); remotePlayer->StopInteract(); }
            qaAimLocked=false;
            Log("[COOP/QA] Cabin guest notebook picked_body=%d in_guest_inventory=%d\n",picked?1:0,guestInv&&guestInv->GetItem("notebook")?1:0);
            qaCabinStep=2; qaCabinAt=qaTime;
        }
    }
    // 2: l'host prende la torcia dal cassetto; 3: apre l'armadietto con la chiave.
    if(qaCabinStep==2 && qaTime>qaCabinAt+2) { take(init->mpPlayer,hostHands,"flashlight"); qaCabinStep=3; qaCabinAt=qaTime; }
    if(qaCabinStep==3) {
        auto* lock=init->mpMapHandler->GetGameEntity("lock1",false);
        if(lock && qaTime<qaCabinAt+0.2f) place(init->mpPlayer,lock,0.8f);
        if(lock) aim(init->mpPlayer,lock);
        if(qaTime>qaCabinAt+1.2f) {
            if(auto* key=hostInv->GetItem("padlock_key")) { init->mpPlayer->SetCurrentItem(key); init->mpPlayer->ChangeState(ePlayerState_UseItem); init->mpPlayer->StartInteract(); }
            qaCabinStep=4; qaCabinAt=qaTime;
        }
    }
    if(qaCabinStep==4 && qaTime>qaCabinAt+2) {
        auto* lock=init->mpMapHandler->GetGameEntity("lock1",false);
        Log("[COOP/QA] Cabin host key opened_locker=%d key_left=%d\n",lock&&!lock->IsActive()?1:0,hostInv->GetItem("padlock_key")?1:0);
        take(remotePlayer,remoteHands,"glowstick");
        Log("[COOP/QA] Cabin inventories host=[%s] guest=[%s]\n",items(hostInv).c_str(),items(guestInv).c_str());
        qaCabinStep=5; qaCabinAt=qaTime;
    }
    // 5: lo script apre l'uscita 6,5 s dopo che il gruppo ha i tre oggetti.
    if(qaCabinStep==5) {
        auto* door=init->mpMapHandler->GetGameEntity("exitdoor",false);
        if(late && levelHints && guestInv && !guestInv->GetItem("notebook") && !hostInv->GetItem("notebook")) {
            Log("[COOP/QA] Cabin notebook hint shown after %.1f s\n",qaTime-qaCabinAt); take(remotePlayer,remoteHands,"notebook"); qaCabinAt=qaTime;
        }
        if(door && door->IsActive()) { Log("[COOP/QA] Cabin exit door enabled after %.1f s\n",qaTime-qaCabinAt); qaCabinStep=6; qaCabinAt=qaTime; }
        else if(qaTime>qaCabinAt+(late && !levelHints?45:15)) { Log("[COOP/QA] FAIL cabin exit door never enabled host=[%s] guest=[%s]\n",items(hostInv).c_str(),items(guestInv).c_str()); qaCabinStep=99; }
    }
    // 6: chi esce guarda la porta e interagisce.
    if(qaCabinStep==6) {
        cPlayer* p=hostExits?init->mpPlayer:remotePlayer; cPlayerHands* h=hostExits?hostHands:remoteHands;
        auto* door=init->mpMapHandler->GetGameEntity("exitdoor",false);
        { PlayerScope scope(init,p,h); qaAimLocked=true; if(door && qaTime<qaCabinAt+0.2f) place(p,door,0.9f); if(door) aim(p,door); }
        if(qaTime>qaCabinAt+1.2f) {
            const bool picked=door && p->GetPickedBody()==door->GetBody(0);
            Log("[COOP/QA] Cabin %s uses exit door picked_body=%d crosshair=%d\n",who,picked?1:0,int(p->GetCrossHairState()));
            { PlayerScope scope(init,p,h); p->StartInteract(); p->StopInteract(); }
            qaAimLocked=false; qaCabinStep=7; qaCabinAt=qaTime;
        }
    }
    if(qaCabinStep==7 && outside && Ready()) { qaCabinStep=8; qaCabinAt=qaTime; }
    if(qaCabinStep==7 && qaTime>qaCabinAt+20) { Log("[COOP/QA] FAIL cabin exit: no map change map=%s\n",g->map.file.c_str()); qaCabinStep=99; }
    if(qaCabinStep==8 && qaTime>qaCabinAt+2) {
        const bool ok=hostInv->GetItem("flashlight") && hostInv->GetItem("notebook") && hostInv->GetItem("glowstick")
            && guestInv && guestInv->GetItem("flashlight") && guestInv->GetItem("notebook") && guestInv->GetItem("glowstick");
        Log("[COOP/QA] %s cabin exit role=host exit_by=%s map=%s host=[%s] guest=[%s]\n",ok?"PASS":"FAIL",who,g->map.file.c_str(),items(hostInv).c_str(),items(guestInv).c_str());
        qaCabinStep=9; qaCabinAt=qaTime;
    }
    if(qaCabinStep==99) { Log("[COOP/QA] FAIL cabin exit role=host\n"); qaCabinStep=100; qaCabinAt=qaTime; }
    if((qaCabinStep==9 || qaCabinStep==100) && qaTime>qaCabinAt+10) init->mpGame->Exit();
    if(qaTime>160) { Log("[COOP/QA] FAIL cabin exit role=host timeout step=%d\n",qaCabinStep); init->mpGame->Exit(); }
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
        if(qaStage==1 && g->map.file=="maps/level01_01_outside.dae" && Ready() && qaTime>4) {
            // Fixture opens the hatch gate; the original exit interaction changes the map.
            auto* exit=init->mpMapHandler->GetGameEntity("exitarea",false);
            if(!exit || !exit->GetBodyNum()) FatalError("[COOP/QA] Missing outside hatch exit\n");
            auto target=exit->GetBody(0)->GetBV()->GetWorldCenter(); exit->SetActive(true);
            player->GetCharacterBody()->SetFeetPosition(target+cVector3f(0,-.5f,.1f));
            player->SetPickedBody(exit->GetBody(0)); player->GetPickRay()->mvPickedPos=target; exit->PlayerInteract(); qaStage=2;
        }
    }
    if(g->map.file!="maps/level01_02_mine_entrance.dae" || (!Ready() && hammerTime==0)) return;
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
            qaStage=3; Log("[COOP/QA] Arrived in hammer level via cabin/outside/hatch epoch=%u\n",g->map.epoch);
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
            lightsSeen=player->GetFlashLight()->IsActive() && remotePlayer->GetFlashLight()->IsActive() && PartnerView().light && PartnerView().light->IsVisible() && PartnerView().flashlight && PartnerView().flashlight->IsVisible();
            if(PartnerView().light) {
                const auto direction=cMath::MatrixMul(PartnerView().light->GetWorldMatrix().GetRotation(),cVector3f(0,0,-1));
                const float alignment=cMath::Vector3Dot(direction,remotePlayer->GetCamera()->GetForward());
                lightsSeen=lightsSeen && alignment>.99f;
                Log("[COOP/QA] Partner flashlight native aim alignment=%.5f\n",alignment);
            }
            Log("[COOP/QA] Both native hammer states=%d flashlight pair=%d shared_flashlight=%d power=%.1f/%.1f lights=%d/%d/%d\n",hammerEquipped?1:0,lightsSeen?1:0,init->mpInventory->GetItem("flashlight")?1:0,player->GetPower(),remotePlayer->GetPower(),player->GetFlashLight()->IsActive(),remotePlayer->GetFlashLight()->IsActive(),PartnerView().light?PartnerView().light->IsVisible():0);
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
        // Host strafes during the client's walk: the partner moves on the client.
        if(hammerTime>40 && hammerTime<48) player->MoveSideways(std::fmod(hammerTime-40,2.0f)<1 ? 1.0f : -1.0f,dt);
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
        if(hammerTime>10 && hammerTime<25) { player->GetCamera()->SetYaw(g->latest.players[1].yaw/1000.0f); player->GetCamera()->SetPitch(g->latest.players[1].pitch/1000.0f); }
        if(hammerTime<6) {
            const auto& s=g->latest.players[1]; player->GetCamera()->SetYaw(s.yaw/1000.0f); player->GetCamera()->SetPitch(s.pitch/1000.0f);
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
            hammerEquipped=player->GetState()==ePlayerState_WeaponMelee && g->latest.players[0].tinderboxes==1 && g->latest.players[1].tinderboxes==1;
            lightsSeen=player->GetFlashLight()->IsActive() && PartnerView().light && PartnerView().light->IsVisible() && PartnerView().flashlight && PartnerView().flashlight->IsVisible();
            Log("[COOP/QA] Replica shared hammer=%d both equipped=%d flashlight pair=%d\n",hammerShared?1:0,hammerEquipped?1:0,lightsSeen?1:0); qaStage=4;
        }
        if(hammerTime>12 && hammerTime<25) {
            player->GetCamera()->SetYaw(g->latest.players[1].yaw/1000.0f); player->GetCamera()->SetPitch(g->latest.players[1].pitch/1000.0f);
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
        const auto& partner=g->latest.players[IsHost()?1:0];
        auto angles=AimAngles(player->GetCamera()->GetPosition(),cVector3f(partner.x/1000.0f,(partner.y+partner.eyeHeight)/1000.0f,partner.z/1000.0f));
        player->GetCamera()->SetYaw(angles.y); player->GetCamera()->SetPitch(angles.x); player->GetCharacterBody()->SetYaw(angles.y);
    }
    // Partner overlap (52-54.5 s): the client walks straight into the host,
    // who stands still. Players share character group 1, so both machines must
    // let them pass through each other without corrections.
    if(hammerTime>51.5f && qaStage<7) {
        if(IsHost()) {
            const auto feet=player->GetCharacterBody()->GetFeetPosition();
            remotePlayer->GetCharacterBody()->SetFeetPosition(feet+cVector3f(0,0,1.3f));
            remotePlayer->GetCamera()->SetYaw(0); remotePlayer->GetCamera()->SetPitch(0); remotePlayer->GetCharacterBody()->SetYaw(0);
            player->GetCamera()->SetYaw(0);
        }
        qaStage=7; partnerClosest=1000; partnerCorrectionMax=0;
    }
    if(hammerTime>52 && hammerTime<54.5f) {
        const auto& partner=g->latest.players[IsHost()?1:0];
        const auto own=IsHost()?player->GetCharacterBody()->GetFeetPosition():init->mpPlayer->GetCharacterBody()->GetFeetPosition();
        const float dx=own.x-partner.x/1000.0f, dz=own.z-partner.z/1000.0f;
        partnerClosest=std::min(partnerClosest,std::sqrt(dx*dx+dz*dz));
        if(IsClient()) { player->GetCamera()->SetYaw(0); player->GetCamera()->SetPitch(0); partnerCorrectionMax=std::max(partnerCorrectionMax,prediction.CorrectionDistance()); }
    }
    if(hammerTime>54.5f && qaStage==7) {
        Log("[COOP/QA] Partner overlap role=%s closest_xz=%.3f correction_max=%.3f\n",IsHost()?"host":"client",partnerClosest,partnerCorrectionMax);
        qaStage=8;
    }
    if(hammerTime>(IsHost()?60:56)) {
        if(IsClient()) Log("[COOP/QA] Movement intervals average_ms=%.2f max_ms=%.2f samples=%u correction_max_m=%.4f max_horizontal_step_m=%.4f step_beyond_host_m=%.4f path_m=%.3f hard_corrections=%u\n",intervalCount?intervalSum*1000/intervalCount:0,intervalMax*1000,intervalCount,correctionMax,stepMax,stepExcessMax,movementDistance,hardCorrections);
        JudgeSteps(true);
        // Network smoothness: steps beyond what the host body itself did (collision
        // impulses, step climbing) and announced teleports are not network errors.
        const bool smooth=IsHost() || (movementDistance>.5f && stepExcessMax<.2f && hardCorrections==0 && intervalMax<.2f);
        const bool ok=hammerShared && hammerEquipped && clientBroke && hostBroke && lightsSeen && smooth && (IsClient() || heldCrateReleased);
        Log("[COOP/QA] %s hammer, independent equipment, both crate attackers and flashlight pair role=%s\n",ok?"PASS":"FAIL",IsHost()?"host":"client"); init->mpGame->Exit();
    }
}

void cCoopNetwork::OnStart(cSoundEntity* sound) {
    if(!IsHost() || !sound || !sound->GetData()) return;
    online::MediaState s; s.kind=online::MediaKind::Sound; s.id=EntityID(sound->GetName()); s.name=sound->GetName(); s.file=sound->GetData()->GetName(); s.flags=1;
    s.transform=Pack(sound->GetWorldMatrix()); s.values[0]=int32_t(sound->GetVolume()*1000);
    for(auto& guest:guests) if(guest->session.Connected() && guest->pendingMedia.size()<256) guest->pendingMedia.push_back(s);
}
void cCoopNetwork::PublishMedia() {
    PublishEnvironment();
    for(auto it=g->pendingMedia.begin();it!=g->pendingMedia.end();) {
        it->epoch=g->map.epoch;
        if(g->session.SendMedia(*it)) it=g->pendingMedia.erase(it); else break;
    }
    auto* world=init->mpGame->GetScene()->GetWorld3D();
    auto sounds=world->GetSoundEntityIterator();
    // Più suoni possono avere lo stesso nome (ogni cigolio di porta è
    // "MoveSound"): i doppioni viaggiano come "nome#n".
    std::map<std::string,int> seen;
    std::set<std::string> present;
    while(sounds.HasNext()) {
        auto* sound=sounds.Next(); online::MediaState s; s.epoch=g->map.epoch; s.kind=online::MediaKind::Sound;
        const int copy=seen[sound->GetName()]++;
        s.name=copy?sound->GetName()+"#"+std::to_string(copy):sound->GetName(); s.id=EntityID(s.name);
        s.file=sound->GetData()->GetName(); s.flags=sound->IsStopped()?0:1;
        // Volume al centesimo: una dissolvenza non genera un invio a ogni fotogramma.
        s.transform=Pack(sound->GetWorldMatrix()); s.values[0]=int32_t(std::lround(sound->GetVolume()*100))*10;
        auto bytes=online::EncodeMedia(s); auto key="s/"+s.name; present.insert(key);
        if(g->mediaBytes[key]!=bytes && g->session.SendMedia(s)) g->mediaBytes[key]=bytes;
    }
    // Suoni distrutti sull'host (per esempio il cigolio di una porta che si è
    // fermata): il client li ferma, altrimenti un loop continuerebbe per sempre.
    for(auto it=g->mediaBytes.begin();it!=g->mediaBytes.end();) {
        if(it->first.compare(0,2,"s/")!=0 || present.count(it->first)) { ++it; continue; }
        online::MediaState gone; gone.epoch=g->map.epoch; gone.kind=online::MediaKind::Sound; gone.name=it->first.substr(2); gone.id=EntityID(gone.name); gone.flags=128;
        if(g->session.SendMedia(gone)) it=g->mediaBytes.erase(it); else break;
    }
    auto* song=init->mpGame->GetSound()->GetMusicHandler()->GetCurrentSong();
    online::MediaState s; s.epoch=g->map.epoch; s.id=1; s.kind=online::MediaKind::Music; s.name="campaign_music";
    if(song) { s.file=song->msFileName; s.flags=(song->mbLoop?2:0)|1; s.values[0]=int32_t(song->mfMaxVolume*1000); } else s.flags=128;
    auto bytes=online::EncodeMedia(s);
    if(g->mediaBytes["music"]!=bytes && g->session.SendMedia(s)) g->mediaBytes["music"]=bytes;
}
void cCoopNetwork::PublishEnvironment() {
    auto* r=init->mpGame->GetGraphics()->GetRenderer3D();
    auto send=[&](online::Presentation e,const std::string& key) {
        e.epoch=g->map.epoch; auto bytes=online::EncodePresentation(e);
        if(g->mediaBytes[key]!=bytes && g->session.SendPresentation(e)) g->mediaBytes[key]=bytes;
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
        if(s.flags&128) {
            // Creato dalla replica: si elimina (prima restava, e le entità del
            // client crescevano). Della mappa: solo fermato, altri oggetti
            // possono usarlo.
            if(sound && replicatedSounds.erase(s.name)) { sound->Stop(false); world->DestroySoundEntity(sound); }
            else if(sound && !sound->IsStopped()) sound->Stop(false);
            return;
        }
        if(!sound && (s.flags&1)) { sound=world->CreateSoundEntity(s.name,s.file,false); if(sound) replicatedSounds.insert(s.name); }
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
    // FPS reali (fotogrammi disegnati) ogni 5 s, anche in gioco singolo, per
    // confrontare la mod con il gioco senza rete nelle stesse condizioni.
    {
        const uint64_t now=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if(fpsWindowStart==0) fpsWindowStart=now;
        else { const uint64_t gap=now-fpsLastFrame; fpsGapMax=std::max(fpsGapMax,gap); if(gap>33334) ++fpsSlowFrames; }
        fpsLastFrame=now; ++fpsFrames;
        if(now-fpsWindowStart>=5000000) {
            const double seconds=(now-fpsWindowStart)/1e6;
            // Suoni: entità presenti e in riproduzione (il mixer ha 32 voci).
            unsigned soundEntities=0, soundsPlaying=0;
            if(auto* world=init->mpGame->GetScene()->GetWorld3D()) {
                auto sounds=world->GetSoundEntityIterator();
                std::string playing;
                while(sounds.HasNext()) { auto* sound=sounds.Next(); ++soundEntities; if(!sound->IsStopped()) { ++soundsPlaying; if(playing.size()<400) playing+=sound->GetName()+"("+sound->GetData()->GetName()+") "; } }
                if(getenv("PENUMBRA_COOP_QA") && soundsPlaying) Log("[COOP/SOUND] role=%s playing=%s\n",IsHost()?"host":IsClient()?"client":"single",playing.c_str());
            }
            Log("[COOP/FPS] role=%s fps=%.1f frame_ms_mean=%.2f frame_ms_max=%.1f frames_over_33ms=%u sound_entities=%u sounds_playing=%u channels_playing=%d\n",IsHost()?"host":IsClient()?"client":"single",
                fpsFrames/seconds,seconds*1000.0/std::max(1u,fpsFrames),fpsGapMax/1000.0,fpsSlowFrames,soundEntities,soundsPlaying,
                int(init->mpGame->GetSound()->GetSoundHandler()->GetWorldEntryList()->size()+init->mpGame->GetSound()->GetSoundHandler()->GetGuiEntryList()->size()));
            fpsWindowStart=now; fpsFrames=0; fpsGapMax=0; fpsSlowFrames=0;
        }
    }
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
        // Before the 40-48 s walk: saving a BMP stalls the game for about a second.
        if(hammerTime>38) save(4,"_both_crates");
        if(hammerTime>50) save(16,"_pair");
        return;
    }
    if(qaTime<15) return;
    if(!(qaScreenshots&1)) { init->mpGame->GetGraphics()->GetLowLevel()->SaveScreenToBMP(path); qaScreenshots|=1; }
    if(qaTime>32 && g->map.file=="maps/level01_01_outside.dae" && !(qaScreenshots&2) &&
       (IsHost() || (init->mpGame->GetGraphics()->GetRenderer3D()->GetFogActive() && init->mpGame->GetGraphics()->GetRenderer3D()->GetSkyBoxActive()))) {
        std::string second(path); second=second.substr(0,second.find_last_of('.'))+"_outside.bmp";
        init->mpGame->GetGraphics()->GetLowLevel()->SaveScreenToBMP(second); qaScreenshots|=2;
    }
}
float CoopPickMass(iPhysicsBody* body) {
    if(!body) return 0;
    if(body->GetMass()!=0) return body->GetMass();
    auto it=gReplicaMass.find(body); return it==gReplicaMass.end()?0:it->second;
}
