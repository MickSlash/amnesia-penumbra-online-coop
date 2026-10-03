// SPDX-License-Identifier: GPL-3.0-or-later
// Native save validation before deserializing a network checkpoint. The old
// disk loader trusts field types and array lengths, which is unsafe on the wire.
namespace {
bool CompressCheckpoint(const char* text, size_t size, std::vector<uint8_t>& output) {
    if(!size || size > online::MaxCheckpointBytes) return false;
    output = {'Z','C','P','1',uint8_t(size >> 24),uint8_t(size >> 16),uint8_t(size >> 8),uint8_t(size)};
    uLongf compressedSize = compressBound(uLong(size));
    output.resize(8+compressedSize);
    if(compress2(output.data()+8,&compressedSize,reinterpret_cast<const Bytef*>(text),uLong(size),1) != Z_OK) { output.clear(); return false; }
    output.resize(8+compressedSize);
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
// Workers use immutable copies: the engine registry/factories and logging stay
// on the game thread. The schema cache below is only accessed on that thread.
struct CheckpointTypes {
    std::map<tString, CheckpointSchema> schemas;
    std::map<tString, tString> parents;
    mutable tString error;
};
void CheckpointLog(const CheckpointTypes* types, const tString& line) {
    if(!types) NetLog(line);
    else if(types->error.empty()) types->error = line;
}
bool CheckpointDerived(tString name, const tString& base, const CheckpointTypes* types = NULL) {
    for(unsigned i = 0; i < 32 && !name.empty(); ++i) {
        if(name == base) return true;
        if(types) {
            const auto found = types->parents.find(name);
            if(found == types->parents.end()) return false;
            name = found->second; continue;
        }
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
bool CheckpointClass(TiXmlElement* node, const tString& expected, unsigned depth, unsigned& count, const CheckpointTypes* types = NULL) {
    if(!node || depth > 48 || ++count > 300000 || !node->Attribute("type")) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    const tString name = node->Attribute("type");
    if(!CheckpointDerived(name,expected,types)) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    const auto* schema = types ? (types->schemas.count(name) ? &types->schemas.at(name) : NULL) : CheckpointGetSchema(name); if(!schema) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    std::map<tString,bool> seen;
    for(auto* child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if(++count > 300000 || !child->Attribute("name")) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
        const tString fieldName = child->Attribute("name"), tag = child->Value();
        const auto found = schema->find(fieldName);
        if(found == schema->end() || !seen.emplace(fieldName,true).second) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
        const auto& f = found->second.field;
        if(f.mMainType == eSerializeMainType_Variable) {
            if(f.mType == eSerializeType_Class) {
                if(tag != "class" || !CheckpointClass(child,found->second.inlineClass,depth+1,count,types)) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            } else if(f.mType == eSerializeType_ClassPointer) {
                tString base = fieldName == "mpSavedMaps" ? "cLuxSavedGameMapCollection" : fieldName == "mpStateData" ? "iLuxPlayerState_SaveData" : "";
                if(base.empty() || tag != "class_ptr" || !CheckpointClass(child,base,depth+1,count,types)) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            } else {
                int type = 0;
                if(tag != "var" || child->QueryIntAttribute("type",&type) != TIXML_SUCCESS || type != f.mType || !CheckpointScalar(child,type)) {
                    CheckpointLog(types,"[NET/RECOVERY] Invalid save scalar: " + name + "." + fieldName);
                    { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
                }
            }
        } else {
            int type = 0; unsigned elements = 0;
            if(tag != (f.mMainType == eSerializeMainType_Array ? "array" : "container") || child->QueryIntAttribute("type",&type) != TIXML_SUCCESS || type != f.mType) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            if(f.mMainType == eSerializeMainType_Container && type == eSerializeType_Class &&
                (!child->Attribute("class_type") || f.msClassName != child->Attribute("class_type"))) return false;
            for(auto* entry = child->FirstChildElement(); entry; entry = entry->NextSiblingElement()) {
                if(++elements > 20000 || ++count > 300000) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
                if(type == eSerializeType_Class || type == eSerializeType_ClassPointer) {
                    const tString base = f.mMainType == eSerializeMainType_Array ? found->second.inlineClass : f.msClassName;
                    if(base.empty() || !CheckpointClass(entry,base,depth+1,count,types)) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
                } else if(!CheckpointScalar(entry,type)) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
            }
            if(f.mMainType == eSerializeMainType_Array && elements != f.mlArraySize) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
        }
    }
    for(const auto& field : *schema)
        if(!seen.count(field.first) && field.second.field.mType != eSerializeType_ClassPointer) { CheckpointLog(types,"[NET/RECOVERY] Save schema rejected at line " + std::to_string(__LINE__)); return false; }
    return true;
}
bool CheckpointParse(const std::vector<uint8_t>& bytes, TiXmlDocument& doc) {
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
    auto* root = doc.RootElement();
    return !doc.Error() && root && tString(root->Value()) == "SaveGame" && root->FirstChildElement("class") &&
        !root->FirstChildElement("class")->NextSiblingElement();
}
bool CheckpointDocument(const std::vector<uint8_t>& bytes, TiXmlDocument& doc, const CheckpointTypes* types = NULL) {
    unsigned count = 0;
    return CheckpointParse(bytes,doc) && CheckpointClass(doc.RootElement()->FirstChildElement("class"),"cLuxSaveGame_SaveData",0,count,types);
}
TiXmlElement* FindCheckpointField(TiXmlElement* element, const char* tag, int type) {
    if(!element) return NULL;
    int value = 0;
    if(tString(element->Value()) == tag && element->QueryIntAttribute("type",&value) == TIXML_SUCCESS && value == type) return element;
    for(auto* child = element->FirstChildElement(); child; child = child->NextSiblingElement())
        if(auto* found = FindCheckpointField(child,tag,type)) return found;
    return NULL;
}
bool TestNativeCheckpointValidation(const TiXmlDocument& original, const CheckpointTypes* types = NULL) {
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
        if(CheckpointDocument(bytes,checked,types)) return false;
    }
    return true;
}
bool TestMissingParticleSave() {
    cEnginePS_SaveData data;
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    data.mvSize = cVector3f(invalid); data.mColor = cColor(invalid);
    for(int r = 0; r < 4; ++r) for(int c = 0; c < 4; ++c) data.m_mtxTransform.m[r][c] = invalid;
    data.mfMinFadeDistanceStart = data.mfMinFadeDistanceEnd = invalid;
    data.mfMaxFadeDistanceStart = data.mfMaxFadeDistanceEnd = invalid;
    data.mlID = 42; data.mbActive = data.mbVisible = data.mbFadeAtDistance = true;
    data.mvEmitterActive.Resize(1); data.mvEmitterActive[0].mbActive = true;
    data.FromPS(NULL);
    TiXmlElement root("Test"); cSerializeClass::SaveToElement(&data,"",&root); unsigned count = 0;
    return data.mlID == -1 && data.msType.empty() && !data.mbActive && !data.mbVisible && !data.mbFadeAtDistance &&
        !data.mvEmitterActive.Size() && CheckpointClass(root.FirstChildElement("class"),"cEnginePS_SaveData",0,count);
}
struct CheckpointWork {
    online::Checkpoint checkpoint;
    TiXmlDocument doc;
    CheckpointTypes types;
    std::vector<tString> names;
    size_t nextName = 0;
    int64_t parseMs = 0, validateMs = 0;
};
bool ParseCheckpointWork(const std::shared_ptr<CheckpointWork>& work) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint8_t> expanded;
    if(!ExpandCheckpoint(work->checkpoint.bytes,expanded)) return false;
    work->checkpoint.bytes = std::move(expanded);
    if(!CheckpointParse(work->checkpoint.bytes,work->doc)) { work->doc.Clear(); return false; }
    std::map<tString,bool> names;
    std::vector<TiXmlElement*> pending = {work->doc.RootElement()};
    while(!pending.empty()) {
        auto* node = pending.back(); pending.pop_back();
        const tString tag = node->Value();
        if((tag == "class" || tag == "class_ptr") && node->Attribute("type"))
            names.emplace(node->Attribute("type"),true);
        if(names.size() > 4096) { work->doc.Clear(); return false; }
        for(auto* child = node->FirstChildElement(); child; child = child->NextSiblingElement()) pending.push_back(child);
    }
    for(const auto& name : names) work->names.push_back(name.first);
    work->parseMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    return true;
}
bool CopyCheckpointType(CheckpointWork& work, const tString& name) {
    const auto* schema = CheckpointGetSchema(name);
    if(!schema) return false;
    work.types.schemas.emplace(name,*schema);
    tString parent = name;
    for(unsigned i = 0; i < 32 && !parent.empty(); ++i) {
        auto* type = cSerializeClass::GetClass(parent); if(!type) return false;
        work.types.parents.emplace(parent,type->msParentName);
        parent = type->msParentName;
    }
    return parent.empty();
}
bool ValidateCheckpointWork(const std::shared_ptr<CheckpointWork>& work, bool testValidation, bool slowTest) {
    const auto start = std::chrono::steady_clock::now();
    // QA can deliberately hold this worker without stopping rendering or ENet.
    if(slowTest) std::this_thread::sleep_for(std::chrono::milliseconds(350));
    unsigned count = 0;
    bool valid = CheckpointClass(work->doc.RootElement()->FirstChildElement("class"),"cLuxSaveGame_SaveData",0,count,&work->types);
    if(valid && testValidation) valid = TestNativeCheckpointValidation(work->doc,&work->types);
    work->doc.Clear(); // release the large DOM on the worker too
    work->validateMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    return valid;
}
}
struct cLuxCheckpointValidation {
    std::shared_ptr<CheckpointWork> work;
    std::future<bool> future;
    uint32_t generation = 0;
    uint64_t context = 0;
    unsigned stage = 0; // parse, copy engine metadata, validate
    int64_t mainMaxUs = 0;
};
struct cLuxCheckpointCapture {
    struct Work {
        TiXmlDocument doc;
        online::Checkpoint checkpoint;
        uint64_t hash = 0;
        int64_t workerMs = 0;
    };
    std::shared_ptr<Work> work;
    std::future<bool> future;
    uint32_t generation = 0;
    uint64_t context = 0;
    int64_t captureUs = 0, serializeUs = 0, mainUs = 0;
};
void cLuxNetwork::UpdateCheckpoint(float timeStep) {
    if(mCheckpointCapture && mCheckpointCapture->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto& job = *mCheckpointCapture;
        bool valid = false;
        try { valid = job.future.get(); } catch(const std::exception&) {}
        const bool current = mSession.GetRole() == online::Role::Host && mSession.Connected() &&
            job.context == mCheckpointContext && job.generation == mSession.Generation() && job.work->checkpoint.epoch == mEpoch;
        if(valid && current) {
            mSendingCheckpoint = std::move(job.work->checkpoint);
            mCheckpointOffset = 0; mCheckpointHash = job.work->hash;
            NetLog("[NET/RECOVERY] Captured campaign checkpoint bytes=" + std::to_string(mSendingCheckpoint.bytes.size()));
        } else NetLog(current ? "[NET/RECOVERY] Campaign checkpoint compression failed or exceeded size limit" : "[NET/RECOVERY] Obsolete host checkpoint worker result discarded");
        NetLog("[NET/PERF] Host checkpoint main_us=" + std::to_string(job.mainUs) + " capture_us=" + std::to_string(job.captureUs) +
            " serialize_us=" + std::to_string(job.serializeUs) + " worker_print_compress_ms=" + std::to_string(job.work->workerMs));
        mCheckpointCapture.reset();
    }
    if(mCheckpointValidation) {
        auto& job = *mCheckpointValidation;
        const auto mainStart = std::chrono::steady_clock::now();
        if(mSmokeSeconds && mRecoveryTest && !mCheckpointStaleTestTriggered && job.stage == 2 &&
            TestFlag("AMNESIA_NET_TEST_STALE_CHECKPOINT") && job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            mCheckpointStaleTestTriggered = true; ++mCheckpointContext;
            NetLog("[NET/TEST] Invalidated in-flight checkpoint context for stale worker regression");
        }
        const bool current = IsClientReplica() && mSession.Connected() && job.context == mCheckpointContext &&
            job.generation == mSession.Generation() && job.work->checkpoint.epoch == mEpoch;
        bool finished = false, valid = false;
        try {
            if(job.stage != 1 && job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                valid = job.future.get();
                if(job.stage == 0 && valid) job.stage = 1;
                else finished = true;
            }
            if(job.stage == 1 && !finished) {
                bool metadataValid = current;
                // Factories and registry access must stay on the game thread.
                // Amortize copying across frames rather than walking the save here.
                while(metadataValid && job.work->nextName < job.work->names.size()) {
                    metadataValid = CopyCheckpointType(*job.work,job.work->names[job.work->nextName++]);
                    if(std::chrono::steady_clock::now()-mainStart >= std::chrono::milliseconds(2)) break;
                }
                if(!metadataValid || job.work->nextName == job.work->names.size()) {
                    const auto work = job.work;
                    const bool testValidation = mRecoveryTest != 0;
                    const bool slowTest = mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_SLOW_CHECKPOINT");
                    job.future = std::async(std::launch::async,[work,metadataValid,testValidation,slowTest]() {
                        if(!metadataValid) { work->doc.Clear(); return false; }
                        return ValidateCheckpointWork(work,testValidation,slowTest);
                    });
                    job.stage = 2;
                }
            }
            if(finished && valid && current) {
                if(mRecoveryTest) NetLog("[NET/TEST] Native checkpoint rejects unknown classes, short vectors and mismatched container factories");
                if(valid) {
                    mRecoveryCheckpoint = std::move(job.work->checkpoint);
                    // Never acknowledge unvalidated or obsolete checkpoint work.
                    mSession.AcknowledgeCheckpoint(mRecoveryCheckpoint.id);
                    NetLog("[NET/RECOVERY] Complete campaign checkpoint received bytes=" + std::to_string(mRecoveryCheckpoint.bytes.size()));
                }
            }
        } catch(const std::exception&) {
            finished = true; valid = false;
        }
        job.mainMaxUs = std::max(job.mainMaxUs,int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-mainStart).count()));
        if(finished) {
            if(!current) NetLog("[NET/RECOVERY] Obsolete checkpoint worker result discarded");
            else if(!valid) {
                if(!job.work->types.error.empty()) NetLog(job.work->types.error);
                NetLog("[NET/RECOVERY] Invalid native save schema; checkpoint discarded");
            }
            NetLog("[NET/PERF] Client checkpoint worker_parse_ms=" + std::to_string(job.work->parseMs) +
                " worker_validation_ms=" + std::to_string(job.work->validateMs) + " main_max_us=" + std::to_string(job.mainMaxUs));
            mCheckpointValidation.reset(); // the future is ready; never join a busy worker in Update
        }
    }
    if(IsClientReplica()) {
        online::Checkpoint received;
        if(!mCheckpointValidation && mSession.ReadCheckpoint(received)) {
            const auto mainStart = std::chrono::steady_clock::now();
            auto job = std::make_shared<cLuxCheckpointValidation>();
            job->work = std::make_shared<CheckpointWork>();
            job->work->checkpoint = std::move(received);
            job->generation = mSession.Generation(); job->context = mCheckpointContext;
            const auto work = job->work;
            try {
                job->future = std::async(std::launch::async,[work]() { return ParseCheckpointWork(work); });
                job->mainMaxUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-mainStart).count();
                mCheckpointValidation = std::move(job);
            } catch(const std::exception&) { NetLog("[NET/RECOVERY] Checkpoint worker unavailable; previous checkpoint retained"); }
        }
        return;
    }
    if(mSession.GetRole() != online::Role::Host || !mSession.IsMapReady(mEpoch) || !mBaselineSent || WaitingForMap() || !HasNetworkPlayers()) return;
    mCheckpointTimer += timeStep;
    if(!mCheckpointCapture && mSendingCheckpoint.bytes.empty() && (mCheckpointTimer >= 20 || mLastCheckpointEpoch != mEpoch)) {
        const auto start = std::chrono::steady_clock::now();
        auto job = std::make_shared<cLuxCheckpointCapture>();
        job->work = std::make_shared<cLuxCheckpointCapture::Work>();
        job->generation = mSession.Generation(); job->context = mCheckpointContext;
        auto* save = gpBase->mpSaveHandler->CreateSaveGameData(); save->mpSavedMaps = gpBase->mpMapHandler->GetSavedMapCollection();
        const auto capturedAt = std::chrono::steady_clock::now();
        auto* root = new TiXmlElement("SaveGame"); job->work->doc.LinkEndChild(root);
        // Snapshot all engine data on the game thread, including borrowed saved
        // maps. The worker receives an owning XML tree with no engine pointers.
        cSerializeClass::SaveToElement(save,"",root);
        save->mpSavedMaps = NULL; hplDelete(save);
        const auto serializedAt = std::chrono::steady_clock::now();
        job->captureUs = std::chrono::duration_cast<std::chrono::microseconds>(capturedAt-start).count();
        job->serializeUs = std::chrono::duration_cast<std::chrono::microseconds>(serializedAt-capturedAt).count();
        job->work->checkpoint.epoch = mEpoch; job->work->checkpoint.id = ++mCheckpointID;
        const auto work = job->work;
        try {
            job->future = std::async(std::launch::async,[work]() {
                const auto start = std::chrono::steady_clock::now();
                TiXmlPrinter printer; work->doc.Accept(&printer);
                const bool valid = CompressCheckpoint(printer.CStr(),printer.Size(),work->checkpoint.bytes);
                if(valid) work->hash = online::CheckpointHash(work->checkpoint.bytes);
                work->doc.Clear();
                work->workerMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
                return valid;
            });
            job->mainUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            mCheckpointCapture = std::move(job);
        } catch(const std::exception&) { NetLog("[NET/RECOVERY] Host checkpoint worker unavailable"); }
        mLastCheckpointEpoch = mEpoch; mCheckpointTimer = 0;
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
        if(!TestMissingParticleSave()) { NetLog("[NET/TEST] FAIL: absent particle retained invalid serialized fields"); return; }
        NetLog("[NET/TEST] Missing particle save clears stale/non-finite fields before checkpoint serialization");
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
