// SPDX-License-Identifier: GPL-3.0-or-later
// Native save validation before deserializing a network checkpoint. The old
// disk loader trusts field types and array lengths, which is unsafe on the wire.
namespace {
bool CompressCheckpoint(const char* text, size_t size, std::vector<uint8_t>& output) {
    if(!size || size > online::MaxCheckpointBytes) return false;
    cBinaryBuffer compressed;
    if(!compressed.CompressAndAdd(const_cast<char*>(text),size,1,false)) return false;
    output = {'Z','C','P','1',uint8_t(size >> 24),uint8_t(size >> 16),uint8_t(size >> 8),uint8_t(size)};
    output.insert(output.end(),compressed.GetDataPointer(),compressed.GetDataPointer()+compressed.GetSize());
    return output.size() <= online::MaxCheckpointBytes;
}
bool ExpandCheckpoint(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    if(input.size() < 9 || std::memcmp(input.data(),"ZCP1",4)) return false;
    const uint32_t size = (uint32_t(input[4]) << 24) | (uint32_t(input[5]) << 16) | (uint32_t(input[6]) << 8) | input[7];
    if(!size || size > online::MaxCheckpointBytes) return false;
    output.resize(size);
    z_stream stream = {};
    stream.next_in = const_cast<Bytef*>(input.data()+8); stream.avail_in = uInt(input.size()-8);
    stream.next_out = output.data(); stream.avail_out = size;
    if(inflateInit(&stream) != Z_OK) { output.clear(); return false; }
    const int result = inflate(&stream,Z_FINISH);
    const bool valid = result == Z_STREAM_END && stream.total_out == size && stream.avail_in == 0;
    inflateEnd(&stream);
    if(!valid) output.clear();
    return valid;
}
struct CheckpointField { cSerializeMemberField field; tString inlineClass; };
using CheckpointSchema = std::map<tString, CheckpointField>;
bool CheckpointDerived(tString name, const tString& base) {
    for(unsigned i = 0; i < 32 && !name.empty(); ++i) {
        if(name == base) return true;
        auto* type = cSerializeClass::GetClass(name); if(!type) return false;
        name = type->msParentName;
    }
    return false;
}
const CheckpointSchema* CheckpointGetSchema(const tString& name) {
    static std::map<tString, CheckpointSchema> schemas;
    auto existing = schemas.find(name); if(existing != schemas.end()) return &existing->second;
    auto* type = cSerializeClass::GetClass(name); if(!type || !type->mpCreateFunc) return NULL;
    iSerializable* object = type->mpCreateFunc();
    CheckpointSchema schema;
    auto it = cSerializeClass::GetMemberFieldIterator(object);
    while(it.HasNext()) {
        const auto* field = it.GetNext(); CheckpointField entry = {*field, ""};
        if(field->mType == eSerializeType_Class && field->mMainType != eSerializeMainType_Container) {
            auto* member = reinterpret_cast<iSerializable*>(reinterpret_cast<char*>(object) + field->mlOffset);
            entry.inlineClass = member->Serialize_GetTopClass();
        }
        schema.emplace(field->msName, entry);
    }
    hplDelete(object); // default factory instances have no loaded save contents
    return &schemas.emplace(name, std::move(schema)).first->second;
}
bool CheckpointScalar(TiXmlElement* node, unsigned type) {
    const char* text = node->Attribute("val"); if(!text || node->FirstChildElement()) return false;
    if(type == eSerializeType_String || type == eSerializeType_WString) return std::strlen(text) <= 1024*1024;
    if(type == eSerializeType_Bool) return std::string(text) == "true" || std::string(text) == "false";
    if(type == eSerializeType_Int32) {
        char* end = NULL; const long long number = std::strtoll(text,&end,10);
        return end && !*end && number >= INT32_MIN && number <= INT32_MAX;
    }
    if(type < eSerializeType_Float32 || type > eSerializeType_Planef) return false;
    const unsigned required = type == eSerializeType_Float32 ? 1 :
        (type == eSerializeType_Vector2l || type == eSerializeType_Vector2f) ? 2 :
        (type == eSerializeType_Vector3l || type == eSerializeType_Vector3f) ? 3 :
        type == eSerializeType_Matrixf ? 16 : 4;
    unsigned components = 0;
    // Includes vector, colour and matrix components; forbid NaN/Inf.
    while(*text) {
        while(*text == ' ') ++text;
        if(!*text) break;
        char* end = NULL; const float number = std::strtof(text,&end);
        if(end == text || !std::isfinite(number)) return false;
        text = end;
        if(++components > required || (*text && *text != ' ')) return false;
    }
    return components == required;
}
bool CheckpointClass(TiXmlElement* node, const tString& expected, unsigned depth, unsigned& count) {
    if(!node || depth > 48 || ++count > 300000 || !node->Attribute("type")) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    const tString name = node->Attribute("type");
    if(!CheckpointDerived(name,expected)) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    const auto* schema = CheckpointGetSchema(name); if(!schema) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    std::map<tString,bool> seen;
    for(auto* child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if(++count > 300000 || !child->Attribute("name")) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
        const tString fieldName = child->Attribute("name"), tag = child->Value();
        const auto found = schema->find(fieldName);
        if(found == schema->end() || !seen.emplace(fieldName,true).second) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
        const auto& f = found->second.field;
        if(f.mMainType == eSerializeMainType_Variable) {
            if(f.mType == eSerializeType_Class) {
                if(tag != "class" || !CheckpointClass(child,found->second.inlineClass,depth+1,count)) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            } else if(f.mType == eSerializeType_ClassPointer) {
                tString base = fieldName == "mpSavedMaps" ? "cLuxSavedGameMapCollection" : fieldName == "mpStateData" ? "iLuxPlayerState_SaveData" : "";
                if(base.empty() || tag != "class_ptr" || !CheckpointClass(child,base,depth+1,count)) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            } else {
                int type = 0;
                if(tag != "var" || child->QueryIntAttribute("type",&type) != TIXML_SUCCESS || type != f.mType || !CheckpointScalar(child,type)) {
                    NetLog("[NET/RECOVERY] Invalid save scalar: " + name + "." + fieldName);
                    { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
                }
            }
        } else {
            int type = 0; unsigned elements = 0;
            if(tag != (f.mMainType == eSerializeMainType_Array ? "array" : "container") || child->QueryIntAttribute("type",&type) != TIXML_SUCCESS || type != f.mType) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            if(f.mMainType == eSerializeMainType_Container && type == eSerializeType_Class &&
                (!child->Attribute("class_type") || f.msClassName != child->Attribute("class_type"))) return false;
            for(auto* entry = child->FirstChildElement(); entry; entry = entry->NextSiblingElement()) {
                if(++elements > 20000 || ++count > 300000) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
                if(type == eSerializeType_Class || type == eSerializeType_ClassPointer) {
                    const tString base = f.mMainType == eSerializeMainType_Array ? found->second.inlineClass : f.msClassName;
                    if(base.empty() || !CheckpointClass(entry,base,depth+1,count)) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
                } else if(!CheckpointScalar(entry,type)) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            }
            if(f.mMainType == eSerializeMainType_Array && elements != f.mlArraySize) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
        }
    }
    for(const auto& field : *schema)
        if(!seen.count(field.first) && field.second.field.mType != eSerializeType_ClassPointer) { NetLog("[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    return true;
}
bool CheckpointDocument(const std::vector<uint8_t>& bytes, TiXmlDocument& doc) {
    if(bytes.empty() || bytes.size() > online::MaxCheckpointBytes) return false;
    const tString text(bytes.begin(),bytes.end());
    if(text.find('\0') != tString::npos || text.find("<!") != tString::npos) return false;
    // Bound nesting before the recursive XML parser sees peer-supplied text.
    unsigned depth = 0, tags = 0;
    for(size_t i = 0; i < text.size(); ++i) if(text[i] == '<') {
        if(++tags > 600000) return false;
        const size_t end = text.find('>',i); if(end == tString::npos || end == i+1) return false;
        if(text[i+1] == '/') { if(!depth) return false; --depth; }
        else if(text[i+1] != '?' && text[end-1] != '/') { if(++depth > 64) return false; }
        i = end;
    }
    if(depth) return false;
    doc.Parse(text.c_str());
    auto* root = doc.RootElement(); unsigned count = 0;
    return !doc.Error() && root && tString(root->Value()) == "SaveGame" && root->FirstChildElement("class") &&
        !root->FirstChildElement("class")->NextSiblingElement() && CheckpointClass(root->FirstChildElement("class"),"cLuxSaveGame_SaveData",0,count);
}
TiXmlElement* FindCheckpointField(TiXmlElement* element, const char* tag, int type) {
    if(!element) return NULL;
    int value = 0;
    if(tString(element->Value()) == tag && element->QueryIntAttribute("type",&value) == TIXML_SUCCESS && value == type) return element;
    for(auto* child = element->FirstChildElement(); child; child = child->NextSiblingElement())
        if(auto* found = FindCheckpointField(child,tag,type)) return found;
    return NULL;
}
bool TestNativeCheckpointValidation(const TiXmlDocument& original) {
    for(int test = 0; test < 3; ++test) {
        TiXmlDocument altered(original);
        auto* root = altered.RootElement();
        if(test == 0) root->FirstChildElement("class")->SetAttribute("type","UnknownCheckpointClass");
        else {
            auto* field = FindCheckpointField(root,test == 1 ? "var" : "container",test == 1 ? eSerializeType_Vector3f : eSerializeType_Class);
            if(!field) return false;
            field->SetAttribute(test == 1 ? "val" : "class_type",test == 1 ? "0" : "UnknownCheckpointClass");
        }
        TiXmlPrinter printer; altered.Accept(&printer);
        std::vector<uint8_t> bytes(printer.CStr(),printer.CStr()+printer.Size()); TiXmlDocument checked;
        if(CheckpointDocument(bytes,checked)) return false;
    }
    return true;
}
}
void cLuxNetwork::UpdateCheckpoint(float timeStep) {
    online::Checkpoint received;
    if(IsClientReplica()) {
        if(mSession.ReadCheckpoint(received)) {
            std::vector<uint8_t> expanded;
            if(!ExpandCheckpoint(received.bytes,expanded)) { NetLog("[NET/RECOVERY] Invalid compressed checkpoint discarded"); return; }
            received.bytes = std::move(expanded);
            TiXmlDocument doc;
            if(!CheckpointDocument(received.bytes,doc)) { NetLog("[NET/RECOVERY] Invalid native save schema; checkpoint discarded"); return; }
            if(mRecoveryTest) {
                static bool checked = false;
                if(!checked) {
                    checked = true;
                    if(!TestNativeCheckpointValidation(doc)) { NetLog("[NET/TEST] FAIL: native checkpoint validation"); return; }
                    NetLog("[NET/TEST] Native checkpoint rejects unknown classes, short vectors and mismatched container factories");
                }
            }
            mRecoveryCheckpoint = std::move(received);
            mSession.AcknowledgeCheckpoint(mRecoveryCheckpoint.id);
            NetLog("[NET/RECOVERY] Complete campaign checkpoint received bytes=" + std::to_string(mRecoveryCheckpoint.bytes.size()));
        }
        return;
    }
    if(mSession.GetRole() != online::Role::Host || !mSession.IsMapReady(mEpoch) || !mBaselineSent || WaitingForMap() || !HasNetworkPlayers()) return;
    mCheckpointTimer += timeStep;
    if(mSendingCheckpoint.bytes.empty() && (mCheckpointTimer >= 20 || mLastCheckpointEpoch != mEpoch)) {
        auto* save = gpBase->mpSaveHandler->CreateSaveGameData(); save->mpSavedMaps = gpBase->mpMapHandler->GetSavedMapCollection();
        TiXmlDocument doc; auto* root = new TiXmlElement("SaveGame"); doc.LinkEndChild(root);
        cSerializeClass::SaveToElement(save,"",root);
        save->mpSavedMaps = NULL; hplDelete(save);
        TiXmlPrinter printer; doc.Accept(&printer);
        if(printer.Size() > online::MaxCheckpointBytes) { NetLog("[NET/RECOVERY] Campaign save exceeds checkpoint transfer limit"); mCheckpointTimer = 0; return; }
        mSendingCheckpoint.epoch = mEpoch; mSendingCheckpoint.id = ++mCheckpointID;
        if(!CompressCheckpoint(printer.CStr(),printer.Size(),mSendingCheckpoint.bytes)) { mCheckpointTimer = 0; return; }
        mCheckpointOffset = 0; mCheckpointHash = online::CheckpointHash(mSendingCheckpoint.bytes);
        mLastCheckpointEpoch = mEpoch; mCheckpointTimer = 0;
        NetLog("[NET/RECOVERY] Captured campaign checkpoint bytes=" + std::to_string(mSendingCheckpoint.bytes.size()));
    }
    // One non-fragmented 1 KiB chunk per 60 Hz tick, on its own bulk channel.
    // Slow links keep movement/audio queues independent of the checkpoint.
    mCheckpointPacer += timeStep;
    if(mCheckpointPacer < 1.0f/60) return;
    mCheckpointPacer = 0;
    for(unsigned i = 0; i < 1 && !mSendingCheckpoint.bytes.empty(); ++i) {
        online::CheckpointChunk chunk; chunk.epoch = mSendingCheckpoint.epoch; chunk.id = mSendingCheckpoint.id;
        chunk.total = uint32_t(mSendingCheckpoint.bytes.size()); chunk.offset = mCheckpointOffset; chunk.hash = mCheckpointHash;
        const size_t end = std::min(size_t(chunk.total),size_t(mCheckpointOffset)+1024);
        chunk.bytes.assign(mSendingCheckpoint.bytes.begin()+mCheckpointOffset,mSendingCheckpoint.bytes.begin()+end);
        if(!mSession.SendCheckpoint(chunk)) break;
        mCheckpointOffset = uint32_t(end);
        if(end == chunk.total) mSendingCheckpoint.bytes.clear();
    }
}
bool cLuxNetwork::PromoteFromCheckpoint() {
    TiXmlDocument doc;
    if(!CheckpointDocument(mRecoveryCheckpoint.bytes,doc)) return false;
    auto* root = doc.RootElement()->FirstChildElement("class");
    TiXmlElement *first = NULL, *second = NULL;
    for(auto* field = root->FirstChildElement("class"); field; field = field->NextSiblingElement("class")) {
        const tString name = field->Attribute("name");
        if(name == "mPlayer") first = field; if(name == "mPlayer2") second = field;
    }
    if(!first || !second) return false;
    // The former client becomes P1 locally; retain the former host as P2.
    first->SetAttribute("name","mPlayer2"); second->SetAttribute("name","mPlayer");
    auto* save = hplNew(cLuxSaveGame_SaveData,()); cSerializeClass::LoadFromElement(save,root);
    online::MapState map; map.epoch = 1; map.file = save->mMap.msFileName; map.fingerprint = MapFingerprint(map.file);
    std::replace(map.file.begin(),map.file.end(),'\\','/');
    if(!save->mbHasPlayer2 || !save->mpSavedMaps || online::EncodeMap(map).empty() || !MapFingerprint(map.file) ||
        save->msMapFolder.find("..") != tString::npos || save->msMapFolder.find(':') != tString::npos || save->msMapFolder.compare(0,5,"maps/")) {
        if(save->mpSavedMaps) hplDelete(save->mpSavedMaps); hplDelete(save); return false;
    }
    const uint16_t port = mSession.ListenPort();
    if(!mSession.StartHost(port,true)) { hplDelete(save->mpSavedMaps); hplDelete(save); return false; }
    mWasClient = mHadClientConnection = false; mEpoch = 0;
    gpBase->mpEngine->GetUpdater()->SetContainer("Default"); gpBase->mpInputHandler->ChangeState(eLuxInputState_Game);
    gpBase->mpSaveHandler->LoadSaveGameData(save); save->mpSavedMaps = NULL; hplDelete(save);
    mRemotePlayer2 = gpBase->mpPlayer2; mMapAnnounced = false;
    // Remain paused until the partner returns; an unattended P2 must not die.
    mRecovering = true; mMigrationHost = true;
    if(mRecoveryTest) { mRecoveryTestFixtures = true; mRecoveryTestInterrupted = true; }
    gpBase->mpSaveHandler->AutoSave();
    NetLog("[NET/RECOVERY] Host migrated from complete campaign checkpoint; waiting for partner");
    if(mRecoveryTest) {
        const bool state = gpBase->mpMapHandler->GetCurrentMap()->GetVar("online_recovery_test")->msVal == "campaign_progress_42";
        const bool players = std::fabs(gpBase->mpPlayer->GetHealth()-73) < 0.01f && std::fabs(gpBase->mpPlayer2->GetHealth()-99) < 0.01f;
        NetLog(state && players ? "[NET/TEST] Migration restored campaign variable and swapped individual player states" : "[NET/TEST] FAIL: migration campaign/player state mismatch");
    }
    return true;
}
void cLuxNetwork::UpdateRecoveryTest() {
    if(!mRecoveryTest || !mSmokeSeconds) return;
    auto* map = gpBase->mpMapHandler->GetCurrentMap(); if(!map) return;
    const auto now = std::chrono::steady_clock::now();
    if(mSession.GetRole() == online::Role::Host && mSession.Connected() && mBaselineSent && gpBase->mpPlayer2 && !mRecoveryTestFixtures && !mMigrationHost) {
        mRecoveryTestFixtures = true;
        gpBase->mpJournal->AddNote("online_recovery_note", "note_letter.tga");
        int index = 0;
        gpBase->mpJournal->AddDiary("online_recovery_diary", "diary_paper.tga",index);
        gpBase->mpJournal->AddDiary("online_recovery_diary", "diary_paper.tga",index);
        gpBase->mpJournal->AddQuestNote("online_recovery_active", "online_recovery_active");
        gpBase->mpJournal->AddQuestNote("online_recovery_done", "online_recovery_done");
        gpBase->mpJournal->DisableQuestNote("online_recovery_done");
        map->GetVar("online_recovery_test")->msVal = "campaign_progress_42";
        gpBase->mpPlayer->SetHealth(99); gpBase->mpPlayer2->SetHealth(73);
        mLastCheckpointEpoch = 0;
        NetLog("[NET/TEST] Recovery fixture created with journal, campaign variable and distinct player states");
    }
    if(mRecoveryTestFixtures && !mRecoveryTestInterrupted && mSession.CheckpointAcknowledged()) {
        mRecoveryTestInterrupted = true; mRecoveryTestAt = now;
        mSession.SuspendTransportForTest();
        NetLog("[NET/TEST] Host transport interrupted after complete checkpoint acknowledgement");
    }
    if(mRecoveryTest == 1 && mRecoveryTestFixtures && mRecoveryTestInterrupted && !mRecoveryTestResumed && now - mRecoveryTestAt >= std::chrono::seconds(2)) {
        mRecoveryTestResumed = mSession.StartHost(mConnectPort,true);
        NetLog("[NET/TEST] Original host listening again after two-second outage");
    }
    if(mAppliedJournal || (mRecoveryTestFixtures && mBaselineSent && mSession.Connected())) {
        const auto journal = gpBase->mpJournal->CaptureNetworkJournal(mEpoch);
        unsigned note = 0, diary = 0, active = 0, complete = 0;
        for(const auto& e : journal.entries) {
            if(e.kind == 0 && e.nameEntry == "Note_online_recovery_note_Name") ++note;
            if(e.kind == 1 && e.group == "online_recovery_diary") ++diary;
            if(e.kind == 2 && e.name == "online_recovery_active" && e.active) ++active;
            if(e.kind == 2 && e.name == "online_recovery_done" && !e.active) ++complete;
        }
        if(note == 1 && diary == 2 && active == 1 && complete == 1 && mSmokeSeconds && mJournalTestLastEpoch != mEpoch) {
            mJournalTestLastEpoch = mEpoch;
            NetLog("[NET/TEST] Journal history verified: one note, two ordered diaries, active and completed quests; epoch=" + std::to_string(mEpoch));
        }
    }
}
