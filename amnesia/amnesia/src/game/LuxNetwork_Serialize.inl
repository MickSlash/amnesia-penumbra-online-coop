// SPDX-License-Identifier: GPL-3.0-or-later
// Same XML layout/value formatter as the native save writer, advanced in small
// slices on the game thread. The caller owns a fixed save snapshot and guards
// the revision of its borrowed historical map collection before each slice.
namespace {
class CheckpointSerializer {
    struct DeleteIterator {
        void operator()(iContainerIterator* value) const { if(value) hplDelete(value); }
    };
    struct Frame {
        enum Kind { Object, Container, Array } kind = Object;
        void* data = NULL;
        TiXmlElement* element = NULL;
        cSerializeMemberFieldIterator fields = cSerializeMemberFieldIterator(NULL);
        std::unique_ptr<iContainerIterator,DeleteIterator> iterator;
        eSerializeType type = eSerializeType_NULL;
        size_t index = 0, count = 0, stride = 0;
    };
    std::vector<Frame> stack;
    static void* At(void* data,size_t offset) { return static_cast<unsigned char*>(data)+offset; }
    static TiXmlElement* Element(TiXmlElement* parent,const char* tag) {
        auto* result = new TiXmlElement(tag); parent->LinkEndChild(result); return result;
    }
    void Object(iSerializable* data,const tString& name,TiXmlElement* parent,bool pointer) {
        if(!data) return; // Native SaveToElement omits null class pointers.
        auto* node = Element(parent,pointer ? "class_ptr" : "class");
        node->SetAttribute("type",data->Serialize_GetTopClass().c_str()); node->SetAttribute("name",name.c_str());
        Frame frame; frame.data=data; frame.element=node; frame.fields=cSerializeClass::GetMemberFieldIterator(data);
        stack.push_back(std::move(frame));
    }
    void Value(void* data,eSerializeType type,TiXmlElement* parent,const tString* name=NULL) {
        auto* node = Element(parent,"var");
        if(name) { node->SetAttribute("type",type); node->SetAttribute("name",name->c_str()); }
        node->SetAttribute("val",cSerializeClass::ValueToString(data,0,type));
    }
    void Item(void* data,eSerializeType type,TiXmlElement* parent) {
        if(type==eSerializeType_Class) Object(static_cast<iSerializable*>(data),"",parent,false);
        else if(type==eSerializeType_ClassPointer) Object(*static_cast<iSerializable**>(data),"",parent,true);
        else Value(data,type,parent);
    }
    void Field(void* owner,cSerializeMemberField* field,TiXmlElement* parent) {
        void* data=At(owner,field->mlOffset);
        if(field->mMainType==eSerializeMainType_Variable) {
            if(field->mType==eSerializeType_Class) Object(static_cast<iSerializable*>(data),field->msName,parent,false);
            else if(field->mType==eSerializeType_ClassPointer) Object(*static_cast<iSerializable**>(data),field->msName,parent,true);
            else Value(data,field->mType,parent,&field->msName);
            return;
        }
        Frame frame; frame.data=data; frame.type=field->mType;
        frame.kind=field->mMainType==eSerializeMainType_Container ? Frame::Container : Frame::Array;
        frame.element=Element(parent,frame.kind==Frame::Container ? "container" : "array");
        frame.element->SetAttribute("type",field->mType); frame.element->SetAttribute("name",field->msName.c_str());
        if(frame.kind==Frame::Container) {
            frame.iterator.reset(cSerializeClass::CreateContainerIterator(static_cast<iContainer*>(data)));
            if(field->mType==eSerializeType_Class) frame.element->SetAttribute("class_type",field->msClassName.c_str());
        } else {
            frame.count=field->mlArraySize; frame.element->SetAttribute("size",int(frame.count));
            if(field->mType==eSerializeType_Class) {
                auto* type=cSerializeClass::GetClass(static_cast<iSerializable*>(data)->Serialize_GetTopClass());
                if(!type) throw std::runtime_error("unknown checkpoint array class"); frame.stride=type->mlSize;
            } else if(field->mType==eSerializeType_ClassPointer) {
                auto* first=*static_cast<iSerializable**>(data);
                if(!first) return; // Match native handling of a null first array pointer.
                frame.element->SetAttribute("class_type",first->Serialize_GetTopClass().c_str()); frame.stride=sizeof(void*);
            } else frame.stride=cSerializeClass::ValueTypeSize(field->mType);
        }
        stack.push_back(std::move(frame));
    }
public:
    CheckpointSerializer(iSerializable* data,TiXmlElement* parent) { stack.reserve(64); Object(data,"",parent,false); }
    bool Step(std::chrono::microseconds budget) {
        if(stack.empty()) return true;
        const auto start=std::chrono::steady_clock::now();
        do {
            auto& frame=stack.back();
            if(frame.kind==Frame::Object) {
                if(!frame.fields.HasNext()) stack.pop_back();
                else { auto* field=frame.fields.GetNext(); void* data=frame.data; auto* node=frame.element; Field(data,field,node); }
            } else if(frame.kind==Frame::Container) {
                if(!cSerializeClass::ContainerHasNext(frame.iterator.get())) stack.pop_back();
                else { void* data=cSerializeClass::ContainerNext(frame.iterator.get()); auto type=frame.type; auto* node=frame.element; Item(data,type,node); }
            } else {
                if(frame.index==frame.count) stack.pop_back();
                else { void* data=At(frame.data,frame.stride*frame.index++); auto type=frame.type; auto* node=frame.element; Item(data,type,node); }
            }
        } while(!stack.empty() && std::chrono::steady_clock::now()-start<budget);
        return stack.empty();
    }
};
}
