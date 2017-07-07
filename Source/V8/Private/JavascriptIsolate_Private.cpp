PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#ifndef THIRD_PARTY_INCLUDES_START
#	define THIRD_PARTY_INCLUDES_START
#	define THIRD_PARTY_INCLUDES_END
#endif

#include "JavascriptIsolate_Private.h"
#include "Templates/Tuple.h"
#include "Config.h"
#include "MallocArrayBufferAllocator.h"
#include "Translator.h"
#include "ScopedArguments.h"
#include "Exception.h"
#include "Delegates.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext_Private.h"
#include "JavascriptContext.h"
#include "Helpers.h"
#include "JavascriptGeneratedClass.h"
#include "JavascriptGeneratedClass_Native.h"
#include "StructMemoryInstance.h"
#include "JavascriptMemoryObject.h"
#include "Engine/UserDefinedStruct.h"
#include "Ticker.h"
#include "V8PCH.h"
#include "UObjectIterator.h"
#include "TextProperty.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif
#include "JavascriptStats.h"
#include "IV8.h"

THIRD_PARTY_INCLUDES_START
#include <libplatform/libplatform.h>
THIRD_PARTY_INCLUDES_END

using namespace v8;

// HACK FOR ACCESS PRIVATE MEMBERS
class hack_private_key {};
static UClass* PlaceholderUClass;

template<>
FObjectInitializer const& FObjectInitializer::SetDefaultSubobjectClass<hack_private_key>(TCHAR const*SubobjectName) const
{
	AssertIfSubobjectSetupIsNotAllowed(SubobjectName);
	ComponentOverrides.Add(SubobjectName, PlaceholderUClass, *this);
	return *this;
}
// END OF HACKING

struct FPrivateJavascriptFunction
{
	Isolate* isolate;
	UniquePersistent<Context> context;
	UniquePersistent<Function> Function;
};

struct FPrivateJavascriptRef
{
	UniquePersistent<Object> Object;
};

template <typename CppType>
struct TStructReader
{	
	UScriptStruct* ScriptStruct;

	TStructReader(UScriptStruct* InScriptStruct)
		: ScriptStruct(InScriptStruct)
	{}

	bool Read(Isolate* isolate, Local<Value> Value, CppType& Target) const;
};

static v8::ArrayBuffer::Contents GCurrentContents;

int32 FArrayBufferAccessor::GetSize()
{
	return GCurrentContents.ByteLength();
}

void* FArrayBufferAccessor::GetData()
{
	return GCurrentContents.Data();
}

void FArrayBufferAccessor::Discard()
{
	GCurrentContents = v8::ArrayBuffer::Contents();
}

FString PropertyNameToString(UProperty* Property)
{
	auto Struct = Property->GetOwnerStruct();
	auto name = Property->GetFName();
	if (Struct)
	{
		if (auto s = Cast<UUserDefinedStruct>(Struct))
		{
			return s->PropertyNameToDisplayName(name);
		}
	}
	return name.ToString();
}

bool MatchPropertyName(UProperty* Property, FName NameToMatch)
{
	auto Struct = Property->GetOwnerStruct();
	auto name = Property->GetFName();
	if (Struct)
	{
		if (auto s = Cast<UUserDefinedStruct>(Struct))
		{
			return s->PropertyNameToDisplayName(name) == NameToMatch.ToString();
		}
	}
	return name == NameToMatch;
}

class FJavascriptIsolateImplementation : public FJavascriptIsolate
{
public:
	FJavascriptContext* GetContext()
	{
		return FJavascriptContext::FromV8(isolate_->GetCurrentContext());
	}

	Persistent<ObjectTemplate> GlobalTemplate;

	// Allocator instance should be set for V8's ArrayBuffer's
	FMallocArrayBufferAllocator AllocatorInstance;	

	IDelegateManager* Delegates;

	FTickerDelegate TickDelegate;
	FDelegateHandle TickHandle;

	struct FObjectPropertyAccessors
	{
		static void* This(Local<Value> self)
		{
			return UObjectFromV8(self);
		}

		static Local<Value> Get(Isolate* isolate, Local<Object> self, UProperty* Property)
		{
			auto Object = UObjectFromV8(self);

			if (IsValid(Object))
			{
				FScopeCycleCounterUObject ContextScope(Object);
				FScopeCycleCounterUObject PropertyScope(Property);				
				SCOPE_CYCLE_COUNTER(STAT_JavascriptPropertyGet);

				if (auto p = Cast<UMulticastDelegateProperty>(Property))
				{
					return GetSelf(isolate)->Delegates->GetProxy(self, Object, p);
				}
				else if (auto p = Cast<UDelegateProperty>(Property))
				{
					return GetSelf(isolate)->Delegates->GetProxy(self, Object, p);
				}
				else
				{
					return ReadProperty(isolate, Property, (uint8*)Object, FObjectPropertyOwner(Object));
				}
			}
			else
			{
				return Undefined(isolate);
			}
		}

		//@TODO : Property-type 'routing' is not necessary!
		static void Set(Isolate* isolate, Local<Object> self, UProperty* Property, Local<Value> value)
		{
			FIsolateHelper I(isolate);

			auto Object = UObjectFromV8(self);

			// Direct access to delegate
			auto SetDelegate = [&](Local<Value> proxy) {
				if (!proxy->IsObject())
				{
					I.Throw(TEXT("Set delegate on invalid instance"));
					return;
				}

				if (!(value->IsFunction() || value->IsNull() || value->IsArray()))
				{
					I.Throw(TEXT("Only [function] or null allowed to set delegate"));
					return;
				}

				auto ProxyObject = proxy->ToObject();
				{					
					auto clear_fn = Handle<Function>::Cast(ProxyObject->Get(I.Keyword("Clear")));
					clear_fn->Call(ProxyObject, 0, nullptr);
				}

				auto add_fn = Handle<Function>::Cast(ProxyObject->Get(I.Keyword("Add")));

				// "whole array" can be set
				if (value->IsArray())
				{
					auto arr = Handle<Array>::Cast(value);
					auto Length = arr->Length();					
					for (decltype(Length) Index = 0; Index < Length; ++Index)
					{						
						auto elem = arr->Get(Index);
						Handle<Value> args[] = {elem};
						add_fn->Call(ProxyObject, 1, args);
					}
				}
				// only one delegate
				else if (!value->IsNull())
				{					
					Handle<Value> args[] = {value};
					add_fn->Call(ProxyObject, 1, args);
				}
			};

			if (IsValid(Object))
			{
				FScopeCycleCounterUObject ContextScope(Object);
				FScopeCycleCounterUObject PropertyScope(Property);
				SCOPE_CYCLE_COUNTER(STAT_JavascriptPropertySet);

				// Multicast delegate
				if (auto p = Cast<UMulticastDelegateProperty>(Property))
				{
					auto proxy = GetSelf(isolate)->Delegates->GetProxy(self, Object, p);
					SetDelegate(proxy);					
				}
				// delegate
				else if (auto p = Cast<UDelegateProperty>(Property))
				{
					auto proxy = GetSelf(isolate)->Delegates->GetProxy(self, Object, p);
					SetDelegate(proxy);
				}
				else
				{
					WriteProperty(isolate, Property, (uint8*)Object, value);
				}				
			}
		}
	};

	struct FStructPropertyAccessors
	{
		static void* This(Local<Value> self)
		{
			return FStructMemoryInstance::FromV8(self)->GetMemory();
		}

		static Local<Value> Get(Isolate* isolate, Local<Object> self, UProperty* Property)
		{
			auto Instance = FStructMemoryInstance::FromV8(self);
			if (Instance)
			{
				return ReadProperty(isolate, Property, Instance->GetMemory(), FStructMemoryPropertyOwner(Instance));
			}
			else
			{
				return Undefined(isolate);
			}
		}

		static void Set(Isolate* isolate, Local<Object> self, UProperty* Property, Local<Value> value)
		{
			FIsolateHelper I(isolate);

			auto Instance = FStructMemoryInstance::FromV8(self);
			if (Instance)
			{
				WriteProperty(isolate, Property, Instance->GetMemory(), value);
			}
			else
			{
				I.Throw(TEXT("Null struct"));
			}
		}
	};

	void RegisterSelf(Isolate* isolate)
	{
		isolate_ = isolate;
		isolate->SetData(0, this);

		Delegates = IDelegateManager::Create(isolate);

#if STATS
		SetupCallbacks();
#endif
	}			

#if STATS
	FCycleCounter Counter[4];	

	void OnGCEvent(bool bStart, GCType type, GCCallbackFlags flags)
	{
		auto GetStatId = [](int Index) -> TStatId {
			switch (Index)
			{
			case 0: return GET_STATID(STAT_Scavenge);
			case 1: return GET_STATID(STAT_MarkSweepCompact);
			case 2: return GET_STATID(STAT_IncrementalMarking);
			default: return GET_STATID(STAT_ProcessWeakCallbacks);
			}
		};

		auto GCEvent = [&](int Index) {
			if (bStart)
			{
				Counter[Index].Start(GetStatId(Index)); 
			}
			else
			{
				Counter[Index].Stop();
			}
		};

		for (int32 Index = 0; Index < ARRAY_COUNT(Counter); ++Index)
		{
			if (type & (1 << Index))
			{
				GCEvent(Index);
			}
		}		
	}

	static void OnMemoryAllocationEvent(ObjectSpace space, AllocationAction action, int size)
	{
		FName StatId;
		switch (space)
		{
		case kObjectSpaceNewSpace: StatId = GET_STATFNAME(STAT_NewSpace); break;
		case kObjectSpaceOldSpace: StatId = GET_STATFNAME(STAT_OldSpace); break;
		case kObjectSpaceCodeSpace: StatId = GET_STATFNAME(STAT_CodeSpace); break;
		case kObjectSpaceMapSpace: StatId = GET_STATFNAME(STAT_MapSpace); break;
		case kObjectSpaceLoSpace: StatId = GET_STATFNAME(STAT_LoSpace); break;
		default: return;
		}

		if (action == kAllocationActionAllocate)
		{
			INC_DWORD_STAT_FNAME_BY(StatId, size);
		}
		else
		{
			DEC_DWORD_STAT_FNAME_BY(StatId, size);
		}		
	}

	void SetupCallbacks()
	{
		isolate_->AddGCEpilogueCallback([](Isolate* isolate, GCType type, GCCallbackFlags flags) {
			GetSelf(isolate)->OnGCEvent(false, type, flags);			
		});		

		isolate_->AddGCPrologueCallback([](Isolate* isolate, GCType type, GCCallbackFlags flags) {
			GetSelf(isolate)->OnGCEvent(true, type, flags);
		});

#if V8_MAJOR_VERSION == 5 && V8_MINOR_VERSION < 3
		isolate_->AddMemoryAllocationCallback([](ObjectSpace space, AllocationAction action,int size) {
			OnMemoryAllocationEvent(space, action, size);
		}, kObjectSpaceAll, kAllocationActionAll);
#endif
	}
#endif

	FJavascriptIsolateImplementation()
	{
		Isolate::CreateParams params;

		// Set our array buffer allocator instance
		params.array_buffer_allocator = &AllocatorInstance;

		// Bind this instance to newly created V8 isolate
		RegisterSelf(Isolate::New(params));

		GenerateBlueprintFunctionLibraryMapping();

		InitializeGlobalTemplate();

		TickDelegate = FTickerDelegate::CreateRaw(this, &FJavascriptIsolateImplementation::HandleTicker);
		TickHandle = FTicker::GetCoreTicker().AddTicker(TickDelegate);
	}

	void InitializeGlobalTemplate()
	{
		// Declares isolate/handle scope
		Isolate::Scope isolate_scope(isolate_);
		HandleScope handle_scope(isolate_);

		Handle<Context> context = Context::New(isolate_);
		Context::Scope ContextScope(context);

		// Create a new object template
		auto ObjectTemplate = ObjectTemplate::New(isolate_);

		// Save it into the persistant handle
		GlobalTemplate.Reset(isolate_, ObjectTemplate);

		// Export all structs
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			ExportStruct(*It);
		}						

		// Export all classes
		for (TObjectIterator<UClass> It; It; ++It)
		{
			ExportClass(*It);
		}

		// Export all enums
		for (TObjectIterator<UEnum> It; It; ++It)
		{
			ExportEnum(*It);
		}

		ExportConsole(ObjectTemplate);

		ExportMemory(ObjectTemplate);

		ExportMisc(ObjectTemplate);		
	}		

	~FJavascriptIsolateImplementation()
	{
		ReleaseAllPersistentHandles();		

		Delegates->Destroy();
		Delegates = nullptr;

		FTicker::GetCoreTicker().RemoveTicker(TickHandle);

		isolate_->Dispose();
	}	

	bool HandleTicker(float DeltaTime)
	{
		auto platform = reinterpret_cast<v8::Platform*>(IV8::Get().GetV8Platform());
		v8::platform::PumpMessageLoop(platform,isolate_);
		return true;
	}

	void ReleaseAllPersistentHandles()
	{
		// Release all exported classes
		ClassToFunctionTemplateMap.Empty();

		// Release all exported structs(non-class)
		ScriptStructToFunctionTemplateMap.Empty();				

		// Release global template
		GlobalTemplate.Reset();
	}

	void GenerateBlueprintFunctionLibraryMapping()
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;

			// Blueprint function library only
			if (Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
			{
				// Iterate over all functions
				for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
				{
					auto Function = *FuncIt;
					TFieldIterator<UProperty> It(Function);

					// It should be a static function 
					if ((Function->FunctionFlags & FUNC_Static) && It)
					{
						// and have first argument to bind with.
						if ((It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
						{
							// The first argument should be type of object
							if (auto p = Cast<UObjectPropertyBase>(*It))
							{
								auto TargetClass = p->PropertyClass;

								// GetWorld() may fail and crash, so target class is bound to UWorld
								if (TargetClass == UObject::StaticClass() && (p->GetName() == TEXT("WorldContextObject") || p->GetName() == TEXT("WorldContext")))
								{
									TargetClass = UWorld::StaticClass();
								}

								BlueprintFunctionLibraryMapping.Add(TargetClass, Function);
								continue;
							}
							else if (auto p = Cast<UStructProperty>(*It))
							{
								BlueprintFunctionLibraryMapping.Add(p->Struct, Function);
								continue;
							}
						}

						// Factory function?
						for (auto It2 = It; It2; ++It2)
						{
							if ((It2->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == (CPF_Parm | CPF_ReturnParm))
							{
								if (auto p = Cast<UStructProperty>(*It2))
								{
									BlueprintFunctionLibraryFactoryMapping.Add(p->Struct, Function);
									break;
								}
							}
						}
					}
				}
			}
		}
	}

	// To tell Unreal engine's GC not to destroy these objects!
	virtual void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector) override
	{
		// All classes
		for (auto It = ClassToFunctionTemplateMap.CreateIterator(); It; ++It)
		{
			UClass* Class = It.Key();
			//UE_LOG(Javascript, Log, TEXT("JavascriptIsolate referencing %s / %s %s (gen by %s %s)"), *(Class->GetOuter()->GetName()), *(Class->GetClass()->GetName()), *(Class->GetName()), Class->ClassGeneratedBy ? *(Class->ClassGeneratedBy->GetClass()->GetName()) : TEXT("none"), Class->ClassGeneratedBy ? *(Class->ClassGeneratedBy->GetName()) : TEXT("none"));
			Collector.AddReferencedObject(Class, InThis);
		}

		// All structs
		for (auto It = ScriptStructToFunctionTemplateMap.CreateIterator(); It; ++It)
		{
			Collector.AddReferencedObject(It.Key(), InThis);
		}
	}	

	Local<Value> InternalReadProperty(UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner)
	{
		FIsolateHelper I(isolate_);

		if (!Buffer)
		{
			I.Throw(TEXT("Read property from invalid memory"));
			return Undefined(isolate_);
		}

		if (auto p = Cast<UIntProperty>(Property))
		{
			return Int32::New(isolate_, p->GetPropertyValue_InContainer(Buffer));
		}
		else if (auto p = Cast<UFloatProperty>(Property))
		{
			return Number::New(isolate_, p->GetPropertyValue_InContainer(Buffer));
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
            return v8::Boolean::New(isolate_, p->GetPropertyValue_InContainer(Buffer));
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{
			auto name = p->GetPropertyValue_InContainer(Buffer);
			return I.Keyword(name.ToString());
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			const FString& Data = p->GetPropertyValue_InContainer(Buffer);
			return V8_String(isolate_, Data);
		}
		else if (auto p = Cast<UTextProperty>(Property))
		{
			const FText& Data = p->GetPropertyValue_InContainer(Buffer);
			return V8_String(isolate_, Data.ToString());
		}		
		else if (auto p = Cast<UClassProperty>(Property))
		{			
			auto Class = Cast<UClass>(p->GetPropertyValue_InContainer(Buffer));

			if (Class)
			{
				return ExportClass(Class)->GetFunction();
			}
			else
			{
				return Null(isolate_);
			}
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			if (auto ScriptStruct = Cast<UScriptStruct>(p->Struct))
			{	
				return ExportStructInstance(ScriptStruct, p->ContainerPtrToValuePtr<uint8>(Buffer), Owner);
			}			
			else
			{
				UE_LOG(Javascript, Warning, TEXT("Non ScriptStruct found : %s"), *p->Struct->GetName());
				
				return Undefined(isolate_);
			}			
		}		
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			FScriptArrayHelper_InContainer helper(p, Buffer);
			auto len = (uint32_t)(helper.Num());
			auto arr = Array::New(isolate_, len);
			auto context = isolate_->GetCurrentContext();

			auto Inner = p->Inner;

			if (Inner->IsA(UStructProperty::StaticClass()))
			{
				uint8* ElementBuffer = (uint8*)FMemory_Alloca(Inner->GetSize());				
				for (decltype(len) Index = 0; Index < len; ++Index)
				{
					Inner->InitializeValue(ElementBuffer);
					Inner->CopyCompleteValueFromScriptVM(ElementBuffer, helper.GetRawPtr(Index));
					if (arr->Set(context, Index, ReadProperty(isolate_, Inner, ElementBuffer, FNoPropertyOwner())).FromMaybe(true)) {} // V8_WARN_UNUSED_RESULT;
					Inner->DestroyValue(ElementBuffer);
				}				
			}
			else
			{
				for (decltype(len) Index = 0; Index < len; ++Index)
				{
					if (arr->Set(context, Index, ReadProperty(isolate_, Inner, helper.GetRawPtr(Index), Owner)).FromMaybe(true)) {} // V8_WARN_UNUSED_RESULT;
				}
			}

			return arr;
		}
		else if (auto p = Cast<UObjectPropertyBase>(Property))
		{
			return ExportObject(p->GetObjectPropertyValue_InContainer(Buffer));
		}				
		else if (auto p = Cast<UByteProperty>(Property))
		{
			auto Value = p->GetPropertyValue_InContainer(Buffer);

			if (p->Enum)
			{							
				return I.Keyword(p->Enum->GetNameStringByIndex(Value));
			}			
			else
			{
				return Int32::New(isolate_, Value);
			}
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			int32 Value = p->GetUnderlyingProperty()->GetValueTypeHash(Buffer);
			return I.Keyword(p->GetEnum()->GetNameStringByIndex(Value));
		}
		else if (auto p = Cast<USetProperty>(Property))
		{
			FScriptSetHelper_InContainer SetHelper(p, Buffer);

			auto Out = Array::New(isolate_);

			auto Num = SetHelper.Num();
			for (int Index = 0; Index < Num; ++Index)
			{
				auto PairPtr = SetHelper.GetElementPtr(Index);

				Out->Set(Index, InternalReadProperty(p->ElementProp, SetHelper.GetElementPtr(Index), Owner));
			}

			return Out;
		}
		else if (auto p = Cast<UMapProperty>(Property))
		{
			FScriptMapHelper_InContainer MapHelper(p, Buffer);

			auto Out = Object::New(isolate_);

			auto Num = MapHelper.Num();
			for (int Index = 0; Index < Num; ++Index)
			{
				uint8* PairPtr = MapHelper.GetPairPtr(Index);

				auto Key = InternalReadProperty(p->KeyProp, PairPtr + p->MapLayout.KeyOffset, Owner);
				auto Value = InternalReadProperty(p->ValueProp, PairPtr, Owner);

				Out->Set(Key, Value);
			}

			return Out;
		}
		else
		{
			return I.Keyword("<Unsupported type>");
		}
	}

	void ReadOffStruct(Local<Object> v8_obj, UStruct* Struct, uint8* struct_buffer)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptReadOffStruct);
		FScopeCycleCounterUObject StructContext(Struct);

		FIsolateHelper I(isolate_);

		MaybeLocal<Array> _arr = v8_obj->GetOwnPropertyNames();
		if (_arr.IsEmpty()) return;

		auto arr = _arr.ToLocalChecked();

		auto len = arr->Length();
		
		for (TFieldIterator<UProperty> PropertyIt(Struct, EFieldIteratorFlags::IncludeSuper); PropertyIt && len; ++PropertyIt)
		{
			auto Property = *PropertyIt;
			auto PropertyName = PropertyNameToString(Property);

			auto name = I.Keyword(PropertyName);
			auto value = v8_obj->Get(name);

			if (!value.IsEmpty() && !value->IsUndefined())
			{
				len--;
				InternalWriteProperty(Property, struct_buffer, value);
			}
		}
	}	

	void InternalWriteProperty(UProperty* Property, uint8* Buffer, Handle<Value> Value)
	{
		FIsolateHelper I(isolate_);

		if (!Buffer)
		{
			I.Throw(TEXT("Write property on invalid memory"));
			return;
		}

		if (Value.IsEmpty() || Value->IsUndefined()) return;

		if (auto p = Cast<UIntProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, Value->Int32Value());
		}
		else if (auto p = Cast<UFloatProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, Value->NumberValue());
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, Value->BooleanValue());
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{			
			p->SetPropertyValue_InContainer(Buffer, FName(*StringFromV8(Value)));
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, StringFromV8(Value));
		}		
		else if (auto p = Cast<UTextProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, FText::FromString(StringFromV8(Value)));
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			if (Value->IsString())
			{
				auto UString = StringFromV8(Value);
				if (UString == TEXT("null"))
				{
					p->SetPropertyValue_InContainer(Buffer, nullptr);
				}
				else
				{
					auto Object = StaticLoadObject(UObject::StaticClass(), nullptr, *UString);					
					if (auto Class = Cast<UClass>(Object))
					{
						p->SetPropertyValue_InContainer(Buffer, Class);
					}
					else if (auto BP = Cast<UBlueprint>(Object))
					{
						auto BPGC = BP->GeneratedClass;
						p->SetPropertyValue_InContainer(Buffer, BPGC);
					}
					else
					{
						p->SetPropertyValue_InContainer(Buffer, Object);
					}
				}
			}
			else
			{
				p->SetPropertyValue_InContainer(Buffer, UClassFromV8(isolate_, Value));
			}
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			if (auto ScriptStruct = Cast<UScriptStruct>(p->Struct))
			{
				auto Instance = FStructMemoryInstance::FromV8(Value);

				// If given value is an instance
				if (Instance)
				{
					auto GivenStruct = Instance->Struct;

					// Type-checking needed
					if (GivenStruct->IsChildOf(ScriptStruct))
					{
						p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(Buffer), Instance->GetMemory());
					}
					else
					{				
						I.Throw(FString::Printf(TEXT("Wrong struct type (given:%s), (expected:%s)"), *GivenStruct->GetName(), *p->Struct->GetName()));
					}
				}
				else if (p->Struct->IsChildOf(FJavascriptFunction::StaticStruct()))
				{
					auto struct_buffer = p->ContainerPtrToValuePtr<uint8>(Buffer);
					FJavascriptFunction func;
					if (Value->IsFunction())
					{
						auto jsfunc = Value.As<Function>();
						func.Handle = MakeShareable(new FPrivateJavascriptFunction);
						func.Handle->isolate = isolate_;
						func.Handle->context.Reset(isolate_, isolate_->GetCurrentContext());
						func.Handle->Function.Reset(isolate_, jsfunc);
					}
					p->Struct->CopyScriptStruct(struct_buffer, &func);
				}
				else if (p->Struct->IsChildOf(FJavascriptRef::StaticStruct()))
				{
					auto struct_buffer = p->ContainerPtrToValuePtr<uint8>(Buffer);
					FJavascriptRef ref;

					if (Value->IsObject())
					{
						auto jsobj = Value.As<Object>();
						ref.Handle = MakeShareable(new FPrivateJavascriptRef);
						ref.Handle->Object.Reset(isolate_, jsobj);
					}
					
					p->Struct->CopyScriptStruct(struct_buffer, &ref);
				}
				// If raw javascript object has been passed,
				else if (Value->IsObject())
				{
					auto v8_obj = Value->ToObject();
					auto struct_buffer = p->ContainerPtrToValuePtr<uint8>(Buffer);

					auto Struct = p->Struct;

					ReadOffStruct(v8_obj, Struct, struct_buffer);
				}
				else
				{
					I.Throw(TEXT("Needed struct data"));
				}
			}
			else
			{
				I.Throw(FString::Printf(TEXT("No ScriptStruct found : %s"), *p->Struct->GetName()));
			}					
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			if (Value->IsArray())
			{
				auto arr = Handle<Array>::Cast(Value);
				auto len = arr->Length();

				FScriptArrayHelper_InContainer helper(p, Buffer);

				// synchronize the length
				auto CurSize = (uint32_t)helper.Num();
				if (CurSize < len)
				{
					helper.AddValues(len - CurSize);
				}
				else if (CurSize > len)
				{
					helper.RemoveValues(len, CurSize - len);
				}

				for (decltype(len) Index = 0; Index < len; ++Index)
				{
					WriteProperty(isolate_, p->Inner, helper.GetRawPtr(Index), arr->Get(Index));
				}
			}
			else
			{
				I.Throw(TEXT("Should write into array by passing an array instance"));
			}
		}
		else if (auto p = Cast<UByteProperty>(Property))
		{
			if (p->Enum)
			{
				auto Str = StringFromV8(Value);
				auto EnumValue = p->Enum->GetIndexByName(FName(*Str), true);
				if (EnumValue == INDEX_NONE)
				{
					I.Throw(FString::Printf(TEXT("Enum Text %s for Enum %s failed to resolve to any value"), *Str, *p->Enum->GetName()));
				}
				else
				{
					p->SetPropertyValue_InContainer(Buffer, EnumValue);
				}
			}			
			else
			{				
				p->SetPropertyValue_InContainer(Buffer, Value->Int32Value());
			}
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			auto Str = StringFromV8(Value);
			auto EnumValue = p->GetEnum()->GetIndexByName(FName(*Str), true);
			if (EnumValue == INDEX_NONE)
			{
				I.Throw(FString::Printf(TEXT("Enum Text %s for Enum %s failed to resolve to any value"), *Str, *p->GetName()));
			}
			else
			{
				uint8* PropData = p->ContainerPtrToValuePtr<uint8>(Buffer);
				p->GetUnderlyingProperty()->SetIntPropertyValue(PropData, (int64)EnumValue);
			}
		}
		else if (auto p = Cast<UObjectPropertyBase>(Property))
		{
			p->SetObjectPropertyValue_InContainer(Buffer, UObjectFromV8(Value));
		}
		else if (auto p = Cast<USetProperty>(Property))
		{
			if (Value->IsArray())
			{
				auto arr = Handle<Array>::Cast(Value);
				auto len = arr->Length();

				FScriptSetHelper_InContainer SetHelper(p, Buffer);

				auto Num = SetHelper.Num();
				for (int Index = 0; Index < Num; ++Index)
				{
					const int32 ElementIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
					uint8* ElementPtr = SetHelper.GetElementPtr(Index);
					InternalWriteProperty(p->ElementProp, ElementPtr, arr->Get(Index));
				}

				SetHelper.Rehash();
			}
		}
		else if (auto p = Cast<UMapProperty>(Property))
		{
			if (Value->IsObject())
			{
				auto v = Value->ToObject();

				FScriptMapHelper_InContainer MapHelper(p, Buffer);

				auto PropertyNames = v->GetOwnPropertyNames();
				auto Num = PropertyNames->Length();
				for (decltype(Num) Index = 0; Index < Num; ++Index) {
					auto Key = PropertyNames->Get(Index);
					auto Value = v->Get(Key);

					auto ElementIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
					MapHelper.Rehash();

					uint8* PairPtr = MapHelper.GetPairPtr(ElementIndex);
					InternalWriteProperty(p->KeyProp, PairPtr + p->MapLayout.KeyOffset, Key);
					InternalWriteProperty(p->ValueProp, PairPtr, Value);
				}
			}
		}
	};		
	
	virtual Local<ObjectTemplate> GetGlobalTemplate() override
	{
		return Local<ObjectTemplate>::New(isolate_, GlobalTemplate);
	}

	void ExportConsole(Local<ObjectTemplate> global_templ)
	{
		FIsolateHelper I(isolate_);

		Local<FunctionTemplate> Template = I.FunctionTemplate();

		auto add_fn = [&](const char* name, FunctionCallback fn) {			
			Template->PrototypeTemplate()->Set(I.Keyword(name), I.FunctionTemplate(fn));
		};

		// console.log
		add_fn("log", [](const FunctionCallbackInfo<Value>& info)
		{
			UE_LOG(Javascript, Log, TEXT("%s"), *StringFromArgs(info));
			info.GetReturnValue().Set(info.Holder());
		});

		// console.warn
		add_fn("warn", [](const FunctionCallbackInfo<Value>& info)
		{
			UE_LOG(Javascript, Warning, TEXT("%s"), *StringFromArgs(info));
			info.GetReturnValue().Set(info.Holder());
		});

		// console.info
		add_fn("info", [](const FunctionCallbackInfo<Value>& info)
		{
			UE_LOG(Javascript, Display, TEXT("%s"), *StringFromArgs(info));
			info.GetReturnValue().Set(info.Holder());
		});

		// console.error
		add_fn("error", [](const FunctionCallbackInfo<Value>& info)
		{
			UE_LOG(Javascript, Error, TEXT("%s"), *StringFromArgs(info));
			info.GetReturnValue().Set(info.Holder());
		});

		// console.assert
		add_fn("assert", [](const FunctionCallbackInfo<Value>& info)
		{
			bool to_assert = info.Length() < 1 || info[0]->IsFalse();
			if (to_assert)
			{
				auto stack_frame = StackTrace::CurrentStackTrace(info.GetIsolate(), 1, StackTrace::kOverview)->GetFrame(0);
				auto filename = stack_frame->GetScriptName();
				auto line_number = stack_frame->GetLineNumber();

				UE_LOG(Javascript, Error, TEXT("Assertion:%s:%d %s"), *StringFromV8(filename), line_number, *StringFromArgs(info, 1));
			}

			info.GetReturnValue().Set(info.Holder());
		});

		// console.void
		add_fn("void", [](const FunctionCallbackInfo<Value>& info)
		{
			info.GetReturnValue().Set(info.Holder());
		});

		global_templ->Set(
			I.Keyword("console"),
			// Create an instance
			Template->GetFunction()->NewInstance()
			);
	}	

	void ExportMisc(Local<ObjectTemplate> global_templ)
	{
		FIsolateHelper I(isolate_);

#if WITH_EDITOR
		auto exec_editor = [](const FunctionCallbackInfo<Value>& info) 
		{
			FEditorScriptExecutionGuard Guard;

			if (info.Length() == 1)
			{
				auto function = info[0].As<Function>();
				if (!function.IsEmpty())
				{
					function->Call(info.This(), 0, nullptr);
				}				
			}
		};
		global_templ->Set(I.Keyword("$execEditor"), I.FunctionTemplate(exec_editor));

		auto exec_transaction = [](const FunctionCallbackInfo<Value>& info)
		{
			if (info.Length() == 2)
			{
				auto String = StringFromV8(info[0]);
				FScopedTransaction Transaction(FText::FromString(String));

				auto function = info[1].As<Function>();
				if (!function.IsEmpty())
				{
					function->Call(info.This(), 0, nullptr);
				}
			}
		};
		global_templ->Set(I.Keyword("$execTransaction"), I.FunctionTemplate(exec_transaction));

		auto exec_profile = [](const FunctionCallbackInfo<Value>& info)
		{
			if (info.Length() == 2)
			{
				for (;;)
				{
					auto function = info[1].As<Function>();
					if (function.IsEmpty())
					{
						break;
					}

					if (info[0]->IsObject())
					{
						auto Object = UObjectFromV8(info[0]);
						if (Object)
						{
							FScopeCycleCounterUObject ContextScope(Object);
							info.GetReturnValue().Set(function->Call(info.This(), 0, nullptr));
							return;
						}
					}

					info.GetReturnValue().Set(function->Call(info.This(), 0, nullptr));
				}
			}
		};
		global_templ->Set(I.Keyword("$profile"), I.FunctionTemplate(exec_profile));
#endif
	}

	void ExportMemory(Local<ObjectTemplate> global_templ)
	{
		FIsolateHelper I(isolate_);

		Local<FunctionTemplate> Template = I.FunctionTemplate();

		auto add_fn = [&](const char* name, FunctionCallback fn) {
			Template->PrototypeTemplate()->Set(I.Keyword(name), I.FunctionTemplate(fn));
		};		

		add_fn("access", [](const FunctionCallbackInfo<Value>& info)
		{
			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);
			
			if (info.Length() == 1)
			{
				auto Source = Cast<UJavascriptMemoryObject>(UObjectFromV8(info[0]));

				if (Source)
				{
					auto ab = ArrayBuffer::New(info.GetIsolate(), Source->GetMemory(), Source->GetSize());
					ab->Set(I.Keyword("$source"), info[0]);
					info.GetReturnValue().Set(ab);
					return;
				}
			}

			I.Throw(TEXT("memory.fork requires JavascriptMemoryObject"));
		});

		add_fn("exec", [](const FunctionCallbackInfo<Value>& info)
		{
			FIsolateHelper I(info.GetIsolate());

			if (info.Length() == 2 && info[0]->IsArrayBuffer() && info[1]->IsFunction())
			{
				auto arr = info[0].As<ArrayBuffer>();
				auto function = info[1].As<Function>();				

				GCurrentContents = arr->GetContents();

				Handle<Value> argv[1];
				argv[0] = arr;
				function->Call(info.This(), 1, argv);

				GCurrentContents = v8::ArrayBuffer::Contents();
			}
			else
			{
				GCurrentContents = v8::ArrayBuffer::Contents();
			}

			info.GetReturnValue().Set(info.Holder());
		});

		// memory.bind
		add_fn("bind", [](const FunctionCallbackInfo<Value>& info)
		{
			UE_LOG(Javascript, Warning, TEXT("memory.bind is deprecated. use memory.exec(ab,fn) instead."));
			FIsolateHelper I(info.GetIsolate());

			if (info.Length() == 1 && info[0]->IsArrayBuffer())
			{
				auto arr = info[0].As<ArrayBuffer>();

				GCurrentContents = arr->Externalize();
			}
			else
			{
				GCurrentContents = v8::ArrayBuffer::Contents();
			}

			info.GetReturnValue().Set(info.Holder());
		});

		// memory.unbind
		add_fn("unbind", [](const FunctionCallbackInfo<Value>& info)
		{
			FIsolateHelper I(info.GetIsolate());

			if (info.Length() == 1 && info[0]->IsArrayBuffer())
			{
				auto arr = info[0].As<ArrayBuffer>();

				if (arr->IsNeuterable())
				{
					arr->Neuter();

					GCurrentContents = v8::ArrayBuffer::Contents();
				}
				else
				{
					I.Throw(TEXT("ArrayBuffer is not neuterable"));
				}
			}
			
			info.GetReturnValue().Set(info.Holder());
		});

		// console.void
		add_fn("write", [](const FunctionCallbackInfo<Value>& info)
		{
			FIsolateHelper I(info.GetIsolate());

			if (info.Length() == 2)
			{
				auto filename = info[0];
				auto data = info[1];

				FArchive* Ar = IFileManager::Get().CreateFileWriter(*StringFromV8(info[0]), 0);
				if (Ar)
				{
					if (data->IsArrayBuffer())
					{
						auto arr = data.As<ArrayBuffer>();
						auto Contents = arr->Externalize();

						Ar->Serialize(Contents.Data(), Contents.ByteLength());
					}

					delete Ar;
				}
			}
			else
			{
				I.Throw(TEXT("Two arguments needed"));
			}

			info.GetReturnValue().Set(info.Holder());
		});

		add_fn("takeSnapshot", [](const FunctionCallbackInfo<Value>& info)
		{			
			FIsolateHelper I(info.GetIsolate());
			class FileOutputStream : public OutputStream
			{
			public:
				FileOutputStream(FArchive* ar) : ar_(ar) {}

				virtual int GetChunkSize() {
					return 65536;  // big chunks == faster
				}

				virtual void EndOfStream() {}

				virtual WriteResult WriteAsciiChunk(char* data, int size) {
					ar_->Serialize(data, size);					
					return ar_->IsError() ? kAbort : kContinue;
				}

			private:
				FArchive* ar_;
			};

			if (info.Length() == 1)
			{
				const HeapSnapshot* const snap = info.GetIsolate()->GetHeapProfiler()->TakeHeapSnapshot();
				FArchive* Ar = IFileManager::Get().CreateFileWriter(*StringFromV8(info[0]), 0);
				if (Ar)
				{
					FileOutputStream stream(Ar);
					snap->Serialize(&stream, HeapSnapshot::kJSON);
					delete Ar;
				}

				// Work around a deficiency in the API.  The HeapSnapshot object is const
				// but we cannot call HeapProfiler::DeleteAllHeapSnapshots() because that
				// invalidates _all_ snapshots, including those created by other tools.
				const_cast<HeapSnapshot*>(snap)->Delete();
			}
			else
			{
				I.Throw(TEXT("One argument needed"));
			}
		});

		global_templ->Set(
			I.Keyword("memory"),
			// Create an instance
			Template->GetFunction()->NewInstance(),
			// Do not modify!
			ReadOnly);
	}
	
	template <typename Fn>
	static Local<Value> CallFunction(Isolate* isolate, Local<Value> self, UFunction* Function, UObject* Object, Fn&& GetArg) 
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptFunctionCallToEngine);

		FIsolateHelper I(isolate);

		EscapableHandleScope handle_scope(isolate);

		// Allocate buffer(param size) in stack
		uint8* Buffer = (uint8*)FMemory_Alloca(Function->ParmsSize);

		// Arguments should construct and destruct along this scope
		FScopedArguments scoped_arguments(Function, Buffer);		

		// Does this function return some parameters by reference?
		bool bHasAnyOutParams = false;

		// Argument index
		int ArgIndex = 0;

		// Intentionally declares iterator outside for-loop scope
		TFieldIterator<UProperty> It(Function);

		int32 NumArgs = 0;

		// Iterate over input parameters
		for (; It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
		{
			auto Prop = *It;

			// Get argument from caller
			auto arg = GetArg(ArgIndex++);

			// Do we have valid argument?
			if (!arg.IsEmpty() && !arg->IsUndefined())
			{				
				WriteProperty(isolate, Prop, Buffer, arg);
			}							

			// This is 'out ref'!
			if ((It->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
			{
				bHasAnyOutParams = true;
			}
		}

		NumArgs = ArgIndex;

		// Call regular native function.
		FScopeCycleCounterUObject ContextScope(Object);
		FScopeCycleCounterUObject FunctionScope(Function);
			
		Object->ProcessEvent(Function, Buffer);

		auto FetchProperty = [&](UProperty* Param, int32 ArgIndex) -> Local<Value> {
			if (auto p = Cast<UStructProperty>(Param))
			{
				// Get argument from caller
				auto arg = GetArg(ArgIndex);

				// Do we have valid argument?
				if (!arg.IsEmpty() && !arg->IsUndefined())
				{
					auto Instance = FStructMemoryInstance::FromV8(arg);
					if (Instance)
					{
						p->Struct->CopyScriptStruct(Instance->GetMemory(), p->ContainerPtrToValuePtr<uint8>(Buffer));

						return arg;
					}
				}
			}

			return ReadProperty(isolate, Param, Buffer, FNoPropertyOwner());
		};

		// In case of 'out ref'
		if (bHasAnyOutParams)
		{
			ArgIndex = 0;

			// Allocate an object to pass return values within
			auto OutParameters = Object::New(isolate);

			// Iterate over parameters again
			for (TFieldIterator<UProperty> It(Function); It; ++It, ArgIndex++)
			{
				UProperty* Param = *It;
				
				auto PropertyFlags = Param->GetPropertyFlags();

				// pass return parameter as '$'
				if (PropertyFlags & CPF_ReturnParm)
				{
					// value can be null if isolate is in trouble
					auto value = FetchProperty(Param, NumArgs);
					if (!value.IsEmpty())
					{
						OutParameters->Set(
							// "$"
							I.Keyword("$"),
							// property value
							value
							);
					}
				}
				// rejects 'const T&' and pass 'T&' as its name
				else if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					auto value = FetchProperty(Param, ArgIndex);
					if (!value.IsEmpty())
					{
						OutParameters->Set(
							// parameter name
							I.Keyword(Param->GetName()),
							// property value
							value
							);
					}
				}
			}

			// We're done
			return handle_scope.Escape(OutParameters);
		}
		else
		{
			// Iterate to fill out return parameter (if we have one)
			for (; It; ++It)
			{
				UProperty* Param = *It;
				if (Param->GetPropertyFlags() & CPF_ReturnParm)
				{
					return handle_scope.Escape(FetchProperty(Param, NumArgs));
				}
			}
		}		

		// No return value available
		return handle_scope.Escape(Undefined(isolate));
	}
		
	void ExportFunction(Handle<FunctionTemplate> Template, UFunction* FunctionToExport)
	{
		FIsolateHelper I(isolate_);

		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](const FunctionCallbackInfo<Value>& info)
		{
			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);

			auto self = info.Holder();

			// Retrieve "FUNCTION"
			auto Function = reinterpret_cast<UFunction*>((Local<External>::Cast(info.Data()))->Value());

			// Determine 'this'
			auto Object = (Function->FunctionFlags & FUNC_Static) ? Function->GetOwnerClass()->ClassDefaultObject : UObjectFromV8(self);

			// Check 'this' is valid
			if (!IsValid(Object))
			{
				I.Throw(FString::Printf(TEXT("Invalid instance for calling a function %s"), *Function->GetName()));
				return;
			}
			
			info.GetReturnValue().Set(
				// Call unreal engine function!
				CallFunction(isolate, self, Function, Object, [&](int ArgIndex) -> Local<Value> {
					// pass an argument if we have
					if (ArgIndex < info.Length())
					{
						return info[ArgIndex];
					}
					// Otherwise, just return undefined.
					else
					{
						return Undefined(isolate);
					}
				})
			);
		};

		auto function_name = I.Keyword(FunctionToExport->GetName());
		auto function = I.FunctionTemplate(FunctionBody, FunctionToExport);

		// In case of static function, you can also call this function by 'Class.Method()'.
		if (FunctionToExport->FunctionFlags & FUNC_Static)
		{
			Template->Set(function_name, function);
		}

		// Register the function to prototype template
		Template->PrototypeTemplate()->Set(function_name, function);
	}

	void ExportBlueprintLibraryFunction(Handle<FunctionTemplate> Template, UFunction* FunctionToExport)
	{
		FIsolateHelper I(isolate_);

		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](const FunctionCallbackInfo<Value>& info)
		{
			auto isolate = info.GetIsolate();

			auto self = info.Holder();

			// Retrieve "FUNCTION"
			auto Function = reinterpret_cast<UFunction*>((Local<External>::Cast(info.Data()))->Value());

			// 'this' should be CDO of owner class
			auto Object = Function->GetOwnerClass()->ClassDefaultObject;						

			info.GetReturnValue().Set(
				// Call unreal engine function!
				CallFunction(isolate, self, Function, Object, [&](int ArgIndex) -> Local<Value> {
					// The first argument is bound automatically
					if (ArgIndex == 0) 
					{
						return self;
					}
					// The rest arguments are being passed like normal function call.
					else if (ArgIndex - 1 < info.Length())
					{
						return info[ArgIndex - 1];
					}					
					else
					{
						return Undefined(isolate);
					}
				})
			);
		};

		auto function_name = I.Keyword(FunctionToExport->GetName());
		auto function = I.FunctionTemplate(FunctionBody, FunctionToExport);
		
		// Register the function to prototype template
		Template->PrototypeTemplate()->Set(function_name, function);		
	}

	void ExportBlueprintLibraryFactoryFunction(Handle<FunctionTemplate> Template, UFunction* FunctionToExport)
	{
		FIsolateHelper I(isolate_);

		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](const FunctionCallbackInfo<Value>& info)
		{
			auto isolate = info.GetIsolate();

			auto self = info.Holder();

			// Retrieve "FUNCTION"
			auto Function = reinterpret_cast<UFunction*>((Local<External>::Cast(info.Data()))->Value());

			// 'this' should be CDO of owner class
			auto Object = Function->GetOwnerClass()->ClassDefaultObject;

			info.GetReturnValue().Set(
				// Call unreal engine function!
				CallFunction(isolate, self, Function, Object, [&](int ArgIndex) -> Local<Value> {
					// pass an argument if we have
					if (ArgIndex < info.Length())
					{
						return info[ArgIndex];
					}
					// Otherwise, just return undefined.
					else
					{
						return Undefined(isolate);
					}
				})
			);
		};

		auto function_name = I.Keyword(FunctionToExport->GetName());
		auto function = I.FunctionTemplate(FunctionBody, FunctionToExport);

		// Register the function to prototype template
		Template->Set(function_name, function);
	}
	
	template <typename PropertyAccessors>
	void ExportProperty(Handle<FunctionTemplate> Template, UProperty* PropertyToExport, int32 PropertyIndex) 
	{
		FIsolateHelper I(isolate_);

		// Property getter
		auto Getter = [](Local<String> property, const PropertyCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();

			auto data = info.Data();
			check(data->IsExternal());

			auto Property = reinterpret_cast<UProperty*>((Local<External>::Cast(data))->Value());
			info.GetReturnValue().Set(PropertyAccessors::Get(isolate, info.This(), Property));			
		};

		// Property setter
		auto Setter = [](Local<String> property, Local<Value> value, const PropertyCallbackInfo<void>& info) {
			auto isolate = info.GetIsolate();

			auto data = info.Data();
			check(data->IsExternal())			

			auto Property = reinterpret_cast<UProperty*>((Local<External>::Cast(data))->Value());
			PropertyAccessors::Set(isolate, info.This(), Property, value);			
		};

		Template->PrototypeTemplate()->SetAccessor(
			I.Keyword(PropertyNameToString(PropertyToExport)),
			Getter, 
			Setter, 
			I.External(PropertyToExport),
			DEFAULT,
			(PropertyAttribute)(DontDelete | (FV8Config::IsWriteDisabledProperty(PropertyToExport) ? ReadOnly : 0))
			);
	}

	void ExportHelperFunctions(UStruct* ClassToExport, Local<FunctionTemplate> Template)
	{
		// Bind blue print library!
		TArray<UFunction*> Functions;
		BlueprintFunctionLibraryMapping.MultiFind(ClassToExport, Functions);

		for (auto Function : Functions)
		{
			ExportBlueprintLibraryFunction(Template, Function);			
		}

		BlueprintFunctionLibraryFactoryMapping.MultiFind(ClassToExport, Functions);
		for (auto Function : Functions)
		{
			ExportBlueprintLibraryFactoryFunction(Template, Function);
		}
	}

	void AddMemberFunction_Class_GetClassObject(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			info.GetReturnValue().Set(GetSelf(isolate)->ForceExportObject(ClassToExport));
		};

		Template->Set(I.Keyword("GetClassObject"), I.FunctionTemplate(fn, ClassToExport));
	}	

	void AddMemberFunction_Class_SetDefaultSubobjectClass(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();
			HandleScope scope(isolate);

			FIsolateHelper I(isolate);

			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto ObjectInitializer = GetSelf(isolate)->GetContext()->GetObjectInitializer();

			if (!ObjectInitializer)
			{
				I.Throw(TEXT("SetDefaultSubobjectClass must be called within ctor"));
				return;
			}

			if (info.Length() < 1)
			{
				I.Throw(TEXT("Missing arg"));
				return;
			}

			auto Class = static_cast<UJavascriptGeneratedClass*>(ObjectInitializer->GetClass());
			if (!Class->JavascriptContext.IsValid())
			{
				I.Throw(TEXT("Fatal"));
				return;
			}

			auto Context = Class->JavascriptContext.Pin();
			auto Name = StringFromV8(info[0]);			
			PlaceholderUClass = ClassToExport;
			ObjectInitializer->SetDefaultSubobjectClass<hack_private_key>(*Name);
			PlaceholderUClass = nullptr;
		};

		Template->Set(I.Keyword("SetDefaultSubobjectClass"), I.FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_CreateDefaultSubobject(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();
			HandleScope scope(isolate);

			FIsolateHelper I(isolate);

			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto ObjectInitializer = GetSelf(isolate)->GetContext()->GetObjectInitializer();

			if (!ObjectInitializer)
			{
				I.Throw(TEXT("CreateDefaultSubobject must be called within ctor"));
				return;
			}

			if (info.Length() < 1)
			{
				I.Throw(TEXT("Missing arg"));
				return;
			}
			
			auto Class = static_cast<UJavascriptGeneratedClass*>(ObjectInitializer->GetClass());
			if (!Class->JavascriptContext.IsValid())
			{
				I.Throw(TEXT("Fatal"));
				return;
			}

			auto Context = Class->JavascriptContext.Pin();
			auto ReturnType = ClassToExport;
			auto Name = StringFromV8(info[0]);
			bool bTransient = info.Length() > 1 ? info[1]->BooleanValue() : false;
			bool bIsRequired = info.Length() > 2 ? info[2]->BooleanValue() : true;
			bool bIsAbstract = info.Length() > 3 ? info[3]->BooleanValue() : false;
			auto Object = ObjectInitializer->CreateDefaultSubobject(ObjectInitializer->GetObj(), *Name, ReturnType, ReturnType, bIsRequired, bIsAbstract, bTransient);

			info.GetReturnValue().Set(Context->ExportObject(Object));
		};

		Template->Set(I.Keyword("CreateDefaultSubobject"), I.FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_GetDefaultSubobjectByName(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			auto Name = StringFromV8(info[0]);

			info.GetReturnValue().Set(GetSelf(isolate)->ExportObject(ClassToExport->GetDefaultSubobjectByName(*Name)));
		};

		Template->Set(I.Keyword("GetDefaultSubobjectByName"), I.FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_GetDefaultObject(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			info.GetReturnValue().Set(GetSelf(isolate)->ExportObject(ClassToExport->GetDefaultObject()));
		};

		Template->Set(I.Keyword("GetDefaultObject"), I.FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_Find(Local<FunctionTemplate> Template, UClass* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);

			if (info.Length() == 2 && info[1]->IsString())
			{
				auto Outer = UObjectFromV8(info[0]);
				auto obj = StaticFindObject(ClassToExport, Outer ? Outer : ANY_PACKAGE, *StringFromV8(info[1]->ToString()));
				auto out = GetSelf(isolate)->ExportObject(obj);
				info.GetReturnValue().Set(out);
			}
			else
			{
				I.Throw(TEXT("Missing resource name to load"));
			}
		};

		Template->Set(I.Keyword("Find"), I.FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_Load(Local<FunctionTemplate> Template, UClass* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);

			if (info.Length() == 1 && info[0]->IsString())
			{
				auto obj = StaticLoadObject(ClassToExport, nullptr, *StringFromV8(info[0]->ToString()));
				auto out = GetSelf(isolate)->ExportObject(obj);
				info.GetReturnValue().Set(out);
			}
			else
			{
				I.Throw(TEXT("Missing resource name to load"));
			}
		};

		Template->Set(I.Keyword("Load"), I.FunctionTemplate(fn, ClassToExport));
	}

	Local<Value> C_Operator(UStruct* StructToExport, Local<Value> Value)
	{
		auto Instance = FStructMemoryInstance::FromV8(Value);

		// If given value is an instance
		if (Instance)
		{
			auto GivenStruct = Instance->Struct;
			if (Instance->Struct->IsChildOf(StructToExport))
			{
				return Value;
			}
		}
		else if (auto ScriptStruct = Cast<UScriptStruct>(StructToExport))
		{
			if (Value->IsObject())
			{
				auto v = Value->ToObject();
				auto Size = ScriptStruct->GetStructureSize();
				auto Target = (uint8*)(FMemory_Alloca(Size));
				FMemory::Memzero(Target, Size);
				ReadOffStruct(v, ScriptStruct, Target);
				return ExportStructInstance(ScriptStruct, Target, FNoPropertyOwner());
			}
		}

		return Local<v8::Value>();
	}

	void AddMemberFunction_Struct_C(Local<FunctionTemplate> Template, UStruct* StructToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto StructToExport = reinterpret_cast<UStruct*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			if (info.Length() == 1)
			{
				info.GetReturnValue().Set(GetSelf(isolate)->C_Operator(StructToExport,info[0]));
			}
		};

		Template->Set(I.Keyword("C"), I.FunctionTemplate(fn, StructToExport));		
	}

	void AddMemberFunction_JavascriptRef_get(Local<FunctionTemplate> Template)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();

			auto self = info.This();
			auto out = Object::New(isolate);

			auto Instance = FStructMemoryInstance::FromV8(self);

			if (Instance->GetMemory())
			{				
				auto Ref = reinterpret_cast<FJavascriptRef*>(Instance->GetMemory());
				if (Ref->Handle.IsValid())
				{
					FPrivateJavascriptRef* Handle = Ref->Handle.Get();
					auto object = Local<Object>::New(isolate, Handle->Object);

					info.GetReturnValue().Set(object);
				}
			}
		};

		Template->PrototypeTemplate()->Set(I.Keyword("get"), I.FunctionTemplate(fn, nullptr));
	}

	void AddMemberFunction_Struct_clone(Local<FunctionTemplate> Template, UStruct* StructToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto Struct = reinterpret_cast<UScriptStruct*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			auto self = info.This();
			auto out = Object::New(isolate);

			auto Instance = FStructMemoryInstance::FromV8(self);

			if (Instance->GetMemory())
			{
				info.GetReturnValue().Set(GetSelf(isolate)->ExportStructInstance(Instance->Struct, Instance->GetMemory(), FNoPropertyOwner()));
			}
		};

		Template->PrototypeTemplate()->Set(I.Keyword("clone"), I.FunctionTemplate(fn, StructToExport));		
	}

	template <typename PropertyAccessor>
	void AddMemberFunction_Struct_toJSON(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);
		
		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto Class = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();
			FIsolateHelper I(isolate);

			auto self = info.This();
			auto out = Object::New(isolate);

			auto Object_toJSON = [&](Local<Value> value) -> Local<Value>
			{
				auto Object = UObjectFromV8(value);
				if (Object == nullptr)
				{
					return Null(isolate);
				}
				else
				{
					return V8_String(isolate, Object->GetPathName());
				}
			};

			for (TFieldIterator<UProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
			{
				auto Property = *PropertyIt;

				if (FV8Config::CanExportProperty(Class, Property))
				{
					auto PropertyName = PropertyNameToString(Property);

					auto name = I.Keyword(PropertyName);
					auto value = PropertyAccessor::Get(isolate, self, Property);
					if (auto p = Cast<UClassProperty>(Property))
					{
						auto Class = UClassFromV8(isolate, value);

						if (Class)
						{
							auto BPGC = Cast<UBlueprintGeneratedClass>(Class);
							if (BPGC)
							{
								auto BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
								value = I.String(BP->GetPathName());
							}
							else
							{
								value = I.String(Class->GetPathName());
							}
						}
						else
						{
							value = I.Keyword("null");
						}

						out->Set(name, value);
					}
					else if (auto p = Cast<UObjectPropertyBase>(Property))
					{
						out->Set(name, Object_toJSON(value));						
					}
					else if (auto p = Cast<UArrayProperty>(Property))
					{
						if (auto q = Cast<UObjectPropertyBase>(p->Inner))
						{
							auto arr = Handle<Array>::Cast(value);
							auto len = arr->Length();
							
							auto out_arr = Array::New(isolate, len);
							out->Set(name, out_arr);

							for (decltype(len) Index = 0; Index < len; ++Index)
							{
								out_arr->Set(Index, Object_toJSON(arr->Get(Index)));								
							}															
						}
						else
						{
							out->Set(name, value);
						}						
					}
					else
					{
						out->Set(name, value);
					}
				}
			}

			info.GetReturnValue().Set(out);
		};

		Template->PrototypeTemplate()->Set(I.Keyword("toJSON"), I.FunctionTemplate(fn, ClassToExport));		
	}

	template <typename PropertyAccessor>
	void AddMemberFunction_Struct_RawAccessor(Local<FunctionTemplate> Template, UStruct* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto Class = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();
			FIsolateHelper I(isolate);

			if (info.Length() != 2 || !info[1]->IsFunction()) return;

			HandleScope handle_scope(isolate);

			const FName PropertyNameToAccess(*StringFromV8(info[0]));
			auto function = info[1].As<Function>();

			auto self = info.This();
			auto Instance = PropertyAccessor::This(self);

			for (TFieldIterator<UProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
			{
				auto Property = *PropertyIt;

				if (auto p = Cast<UArrayProperty>(Property))
				{
					FScriptArrayHelper_InContainer helper(p, Instance);

					if (FV8Config::CanExportProperty(Class, Property) && MatchPropertyName(Property,PropertyNameToAccess))
					{
						Handle<Value> argv[1];

						argv[0] = ArrayBuffer::New(info.GetIsolate(), helper.GetRawPtr(), helper.Num() * p->Inner->GetSize());

						auto out = function->Call(info.This(), 1, argv);
						info.GetReturnValue().Set(out);
						break;
					}
				}				
			}
		};

		Template->PrototypeTemplate()->Set(I.Keyword("$memaccess"), I.FunctionTemplate(fn, ClassToExport));
	}

	Local<FunctionTemplate> InternalExportClass(UClass* ClassToExport)
	{
		FIsolateHelper I(isolate_);

		EscapableHandleScope handle_scope(isolate_);

		auto ConstructorBody = [](const FunctionCallbackInfo<Value>& info)
		{
			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);

			auto ClassToExport = reinterpret_cast<UClass*>((Local<External>::Cast(info.Data()))->Value());			

			if (info.IsConstructCall())
			{
				auto self = info.This();

				UObject* Associated = nullptr;

				// Called by system (via ExportObject)
				if (info.Length() == 1 && info[0]->IsExternal())
				{
					auto ext = Local<External>::Cast(info[0]);

					Associated = reinterpret_cast<UObject*>(ext->Value());

					if (!Associated->IsValidLowLevel())
					{
						Associated = nullptr;
					}
				}

				// Called by user (via 'new' operator)
				if (!Associated)
				{
					const bool bIsJavascriptClass =
						ClassToExport->GetClass()->IsChildOf(UJavascriptGeneratedClass::StaticClass()) ||
						ClassToExport->GetClass()->IsChildOf(UJavascriptGeneratedClass_Native::StaticClass());

					auto PreCreate = [&]() {
						if (bIsJavascriptClass)
						{
							GetSelf(isolate)->ObjectUnderConstructionStack.Push(FPendingClassConstruction(self, ClassToExport));
						}
					};

					// Custom constructors
					if (ClassToExport->IsChildOf(AActor::StaticClass()))
					{
						if (info.Length() == 0)
						{
							I.Throw(TEXT("Missing world to spawn"));
							return;
						}

						auto World = Cast<UWorld>(UObjectFromV8(info[0]));
						if (!World)
						{
							I.Throw(TEXT("Missing world to spawn"));
							return;
						}
						
						FVector Location(ForceInitToZero);
						FRotator Rotation(ForceInitToZero);

						UPackage* CoreUObjectPackage = UObject::StaticClass()->GetOutermost();
						static UScriptStruct* VectorStruct = FindObjectChecked<UScriptStruct>(CoreUObjectPackage, TEXT("Vector"));
						static UScriptStruct* RotatorStruct = FindObjectChecked<UScriptStruct>(CoreUObjectPackage, TEXT("Rotator"));
						static TStructReader<FVector> VectorReader(VectorStruct);
						static TStructReader<FRotator> RotatorReader(RotatorStruct);

						FActorSpawnParameters SpawnInfo;
						switch (FMath::Min(2, info.Length())) {
						case 2:
							if (!VectorReader.Read(isolate, info[1], Location)) return;
							if (info.Length() == 2) break;
						case 3:
							if (!RotatorReader.Read(isolate, info[2], Rotation)) return;
							if (info.Length() == 3) break;
						case 4:
							SpawnInfo.Name = FName(*StringFromV8(info[3]));
							if (info.Length() == 4) break;
						case 5:
							SpawnInfo.ObjectFlags = RF_Transient | RF_Transactional;
							break;
						default:
							break;
						}

						PreCreate();
						Associated = World->SpawnActor(ClassToExport, &Location, &Rotation, SpawnInfo);
#if WITH_EDITOR
						if (SpawnInfo.Name != NAME_None)
							(Cast<AActor>(Associated))->SetActorLabel(StringFromV8(info[3]));
#endif
					}
					else
					{
						UObject* Outer = GetTransientPackage();
						FName Name = NAME_None;
						EObjectFlags ObjectFlags = RF_NoFlags;

						if (info.Length() > 0)
						{
							if (auto value = UObjectFromV8(info[0]))
							{
								Outer = value;
							}
							if (info.Length() > 1)
							{
								Name = FName(*StringFromV8(info[1]));
							}
							if (info.Length() > 2)
							{
								ObjectFlags = (EObjectFlags)(info[2]->Int32Value());
							}
						}

						PreCreate(); 
						Associated = NewObject<UObject>(Outer, ClassToExport, Name, ObjectFlags);
					}

					if (bIsJavascriptClass)
					{
						const auto& Last = GetSelf(isolate)->ObjectUnderConstructionStack.Last();

						bool bSafeToQuit = Last.bCatched;

						GetSelf(isolate)->ObjectUnderConstructionStack.Pop();

						if (bSafeToQuit)
						{
							return;
						}
					}

					if (!Associated)
					{
						I.Throw(TEXT("Failed to spawn"));
						return;
					}
				}

				FPendingClassConstruction(self, ClassToExport).Finalize(GetSelf(isolate), Associated);
			}			
			else
			{
				info.GetReturnValue().Set(GetSelf(isolate)->C_Operator(ClassToExport, info[0]));
			}
		};
		
		auto Template = I.FunctionTemplate(ConstructorBody, ClassToExport);
		Template->InstanceTemplate()->SetInternalFieldCount(1);
		
		AddMemberFunction_Struct_C(Template, ClassToExport);

		// load
		if (!ClassToExport->IsChildOf(AActor::StaticClass()))
		{
			AddMemberFunction_Class_Load(Template, ClassToExport);
		}
		AddMemberFunction_Class_Find(Template, ClassToExport);

		AddMemberFunction_Class_GetClassObject(Template, ClassToExport);
		AddMemberFunction_Class_CreateDefaultSubobject(Template, ClassToExport);
		AddMemberFunction_Class_SetDefaultSubobjectClass(Template, ClassToExport);

		AddMemberFunction_Class_GetDefaultObject(Template, ClassToExport);
		AddMemberFunction_Class_GetDefaultSubobjectByName(Template, ClassToExport);
		
		AddMemberFunction_Struct_toJSON<FObjectPropertyAccessors>(Template, ClassToExport);
		AddMemberFunction_Struct_RawAccessor<FObjectPropertyAccessors>(Template, ClassToExport);

		Template->SetClassName(I.Keyword(ClassToExport->GetName()));

		auto static_class = I.Keyword("StaticClass");

		// access thru Class.prototype.StaticClass
		Template->PrototypeTemplate()->Set(static_class, I.External(ClassToExport));
		Template->Set(static_class, I.External(ClassToExport));		

		for (TFieldIterator<UFunction> FuncIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (FV8Config::CanExportFunction(ClassToExport, Function))
			{
				ExportFunction(Template, Function);
			}
		}		

		int32 PropertyIndex = 0;
		for (TFieldIterator<UProperty> PropertyIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt, ++PropertyIndex)
		{
			UProperty* Property = *PropertyIt;
			if (FV8Config::CanExportProperty(ClassToExport, Property))
			{
				ExportProperty<FObjectPropertyAccessors>(Template, Property, PropertyIndex);
			}
		}

		return handle_scope.Escape(Template);
	}	

	Local<FunctionTemplate> InternalExportStruct(UScriptStruct* StructToExport)
	{
		FIsolateHelper I(isolate_);

		EscapableHandleScope handle_scope(isolate_);

		auto fn = [](const FunctionCallbackInfo<Value>& info)
		{
			auto StructToExport = reinterpret_cast<UScriptStruct*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);

			if (info.IsConstructCall())
			{
				auto self = info.This();

				TSharedPtr<FStructMemoryInstance> Memory;

				if (info.Length() == 2 && info[0]->IsExternal() && info[1]->IsExternal())
				{
					IPropertyOwner& Owner = *reinterpret_cast<IPropertyOwner*>(Local<External>::Cast(info[1])->Value());

					Memory = FStructMemoryInstance::Create(StructToExport, Owner, Local<External>::Cast(info[0])->Value());
				}
				else
				{
					Memory = FStructMemoryInstance::Create(StructToExport, FNoPropertyOwner());
				}

				GetSelf(isolate)->RegisterScriptStructInstance(Memory, self);

				self->SetAlignedPointerInInternalField(0, Memory.Get());
			}
			else
			{
				info.GetReturnValue().Set(GetSelf(isolate)->C_Operator(StructToExport, info[0]));
			}
		};
				
		auto Template = I.FunctionTemplate(fn, StructToExport);
		Template->InstanceTemplate()->SetInternalFieldCount(1);

		AddMemberFunction_Struct_C(Template, StructToExport);
		AddMemberFunction_Struct_clone(Template, StructToExport);
		AddMemberFunction_Struct_toJSON<FStructPropertyAccessors>(Template, StructToExport);
		AddMemberFunction_Struct_RawAccessor<FStructPropertyAccessors>(Template, StructToExport);

		if (StructToExport == FJavascriptRef::StaticStruct())
		{
			AddMemberFunction_JavascriptRef_get(Template);
		}

		Template->SetClassName(I.Keyword(StructToExport->GetName()));

		auto static_class = I.Keyword("StaticClass");

		// access thru Class.prototype.StaticClass
		Template->PrototypeTemplate()->Set(static_class, I.External(StructToExport));
		Template->Set(static_class, I.External(StructToExport));

		int32 PropertyIndex = 0;
		for (TFieldIterator<UProperty> PropertyIt(StructToExport, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt, ++PropertyIndex)
		{
			UProperty* Property = *PropertyIt;
			if (FV8Config::CanExportProperty(StructToExport, Property))
			{
				ExportProperty<FStructPropertyAccessors>(Template, Property, PropertyIndex);
			}
		}

		return handle_scope.Escape(Template);
	}
	
	virtual Local<FunctionTemplate> ExportStruct(UScriptStruct* ScriptStruct) override
	{
		auto ExportedFunctionTemplatePtr = ScriptStructToFunctionTemplateMap.Find(ScriptStruct);
		if (ExportedFunctionTemplatePtr == nullptr)
		{				
			auto Template = InternalExportStruct(ScriptStruct);

			auto SuperStruct = Cast<UScriptStruct>(ScriptStruct->GetSuperStruct());
			if (SuperStruct)
			{
				Template->Inherit(ExportStruct(SuperStruct));
			}				

			ExportHelperFunctions(ScriptStruct, Template);

			RegisterStruct(ScriptStructToFunctionTemplateMap, ScriptStruct, Template);

			return Template;
		}
		else
		{
			return Local<FunctionTemplate>::New(isolate_, *ExportedFunctionTemplatePtr);
		}
	}	

	Local<Value> ExportEnum(const UEnum* enumToExport)
	{
		int32 numMembers = enumToExport->NumEnums();
		auto arr = Array::New(isolate_, numMembers);

		for (int32 i = 0; i < numMembers; ++i)
		{
			FString memberName = enumToExport->GetNameStringByIndex(i);
			if (!memberName.IsEmpty())
			{
				auto memberKey = V8_KeywordString(isolate_, memberName);
				arr->Set(i, memberKey);
				arr->Set(memberKey, memberKey);
			}
		}

		auto enumName = V8_KeywordString(isolate_, FV8Config::Safeify(enumToExport->GetName()));
		GetGlobalTemplate()->Set(enumName, arr);

		return arr;
	}

	virtual Local<FunctionTemplate> ExportClass(UClass* Class, bool bAutoRegister = true) override
	{
		auto ExportedFunctionTemplatePtr = ClassToFunctionTemplateMap.Find(Class);
		if (ExportedFunctionTemplatePtr == nullptr)
		{
			auto Template = InternalExportClass(Class);

			auto SuperClass = Class->GetSuperClass();
			if (SuperClass)
			{
				Template->Inherit(ExportClass(SuperClass));
			}

			ExportHelperFunctions(Class, Template);

			if (bAutoRegister)
			{
				RegisterClass(Class, Template);
			}			
			
			return Template;
		}
		else
		{
			return Local<FunctionTemplate>::New(isolate_, *ExportedFunctionTemplatePtr);
		}
	}

	Local<Value> ExportStructInstance(UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner)
	{
		FIsolateHelper I(isolate_);
		if (!Struct || !Buffer)
		{
			return Undefined(isolate_);
		}

		auto v8_struct = ExportStruct(Struct);
		auto arg = I.External(Buffer);
		auto arg2 = I.External((void*)&Owner);
		Handle<Value> args[] = { arg, arg2 };

		return v8_struct->GetFunction()->NewInstance(2, args);
	}

	Local<Value> ForceExportObject(UObject* Object)
	{
		FIsolateHelper I(isolate_);
		if (!Object)
		{
			return Undefined(isolate_);
		}

		auto ObjectPtr = GetContext()->ObjectToObjectMap.Find(Object);
		if (ObjectPtr == nullptr)
		{
			Local<Value> value;

			auto v8_class = ExportClass(Object->GetClass());
			auto arg = I.External(Object);
			Handle<Value> args[] = { arg };

			value = v8_class->GetFunction()->NewInstance(1, args);

			return value;
		}
		else
		{
			return Local<Value>::New(isolate_, *ObjectPtr);
		}
	}

	Local<Value> ExportObject(UObject* Object, bool bForce = false) override
	{
		if (bForce) return ForceExportObject(Object);

		FIsolateHelper I(isolate_);
		if (!Object)
		{
			return Undefined(isolate_);
		}

		auto Context = GetContext();
		if (!Context)
		{
			return Undefined(isolate_);
		}

		auto ObjectPtr = Context->ObjectToObjectMap.Find(Object);
		if (ObjectPtr == nullptr)
		{
			if (ObjectUnderConstructionStack.Num() > 0)
			{
				auto& Last = ObjectUnderConstructionStack.Last();
				if (!Last.bCatched)
				{
					if (!Object->HasAnyFlags(RF_ClassDefaultObject) && Object->IsA(Last.Class))
					{
						Last.bCatched = true;
						Last.Finalize(this, Object);
						return Last.Object;
					}					
				}
			}
			Local<Value> value;
			
			if (auto Class = Cast<UClass>(Object))
			{
				value = ExportClass(Class)->GetFunction();
			}
			else if (auto Struct = Cast<UScriptStruct>(Object))
			{
				value = ExportStruct(Struct)->GetFunction();
			}
			else
			{
				auto Class = Object->GetClass();
				//if (Class->ClassGeneratedBy && Cast<ULevel>(Class->ClassGeneratedBy->GetOuter()))
				//{
				//	return Undefined(isolate_);
				//}

				auto v8_class = ExportClass(Class);
				auto arg = I.External(Object);
				Handle<Value> args[] = { arg };

				value = v8_class->GetFunction()->NewInstance(1, args);
			}

			return value;
		}
		else
		{
			return Local<Value>::New(isolate_, *ObjectPtr);
		}
	}

	// For tracking exported entities
	template <typename U, typename T>
	void SetWeak(UniquePersistent<U>& Handle, T* GarbageCollectedObject)
	{		
		typedef TPair<FJavascriptContext*, T*> WeakData;
		typedef typename WeakData::KeyType WeakDataKeyInitType;
		typedef typename WeakData::ValueType WeakDataValueInitType;
		typedef TPairInitializer<WeakDataKeyInitType, WeakDataValueInitType> InitializerType;

#if V8_MAJOR_VERSION == 5 && V8_MINOR_VERSION < 3
		Handle.template SetWeak<WeakData>(new WeakData(InitializerType(GetContext(), GarbageCollectedObject)), [](const WeakCallbackData<U, WeakData>& data) {
			auto Parameter = data.GetParameter();

			auto Context = Parameter->Key;
			auto Self = static_cast<FJavascriptIsolateImplementation*>(Context->Environment.Get());
			Self->OnGarbageCollectedByV8(Context,Parameter->Value);

			delete Parameter;
		});
#else
		Handle.template SetWeak<WeakData>(new WeakData(InitializerType(GetContext(), GarbageCollectedObject)), [](const WeakCallbackInfo<WeakData>& data) {
			auto Parameter = data.GetParameter();

			auto Context = Parameter->Key;
			auto Self = static_cast<FJavascriptIsolateImplementation*>(Context->Environment.Get());
			Self->OnGarbageCollectedByV8(Context, Parameter->Value);

			delete Parameter;
		}, WeakCallbackType::kParameter);
#endif
	}

	template <typename StructType>
	void RegisterStruct(TMap< StructType*, v8::UniquePersistent<v8::FunctionTemplate> >& TheMap, StructType* Class, Local<FunctionTemplate> Template)
	{
		FIsolateHelper I(isolate_);

		// public name
		auto name = I.Keyword(FV8Config::Safeify(Class->GetName()));

		// If we are running in a context, we also register this class to the context directly.
		auto Context = isolate_->GetCurrentContext();
		if (!Context.IsEmpty())
		{
			Context->Global()->Set(name, Template->GetFunction());
		}

		// Register this class to the global template so that any other contexts which will be created later have this function template.
		GetGlobalTemplate()->Set(name, Template);

		// Track this class from v8 gc.
		auto& result = TheMap.Add(Class, UniquePersistent<FunctionTemplate>(isolate_, Template));
		SetWeak(result, Class);
	}

	virtual void RegisterClass(UClass* Class, Local<FunctionTemplate> Template) override
	{
		RegisterStruct(ClassToFunctionTemplateMap, Class, Template);		
	}

	void RegisterScriptStruct(UScriptStruct* Struct, Local<FunctionTemplate> Template)
	{
		RegisterStruct(ScriptStructToFunctionTemplateMap, Struct, Template);		
	}

	void RegisterObject(UObject* UnrealObject, Local<Value> value)
	{		
		auto& result = GetContext()->ObjectToObjectMap.Add(UnrealObject, UniquePersistent<Value>(isolate_, value));
		SetWeak(result, UnrealObject);		
	}				

	void RegisterScriptStructInstance(TSharedPtr<FStructMemoryInstance> MemoryObject, Local<Value> value)
	{
		auto context = GetContext();
		auto& result = context->MemoryToObjectMap.Add(MemoryObject, UniquePersistent<Value>(isolate_, value));
		SetWeak(result, MemoryObject.Get());
	}

	void OnGarbageCollectedByV8(FJavascriptContext* Context, FStructMemoryInstance* Memory)
	{
		// We should keep ourselves clean
		Context->MemoryToObjectMap.Remove(Memory->AsShared());
	}

	void OnGarbageCollectedByV8(FJavascriptContext* Context, UObject* Object)
	{
		if (auto klass = Cast<UClass>(Object))
		{
			ClassToFunctionTemplateMap.Remove(klass);
		}

		Context->ObjectToObjectMap.Remove(Object);		
	}	

	static FJavascriptIsolateImplementation* GetSelf(Isolate* isolate)
	{
		return reinterpret_cast<FJavascriptIsolateImplementation*>(isolate->GetData(0));
	}
};

FJavascriptIsolate* FJavascriptIsolate::Create()
{
	return new FJavascriptIsolateImplementation();
}

Local<Value> FJavascriptIsolate::ReadProperty(Isolate* isolate, UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner)
{
	return FJavascriptIsolateImplementation::GetSelf(isolate)->InternalReadProperty(Property, Buffer, Owner);
}

void FJavascriptIsolate::WriteProperty(Isolate* isolate, UProperty* Property, uint8* Buffer, Handle<Value> Value)
{
	FJavascriptIsolateImplementation::GetSelf(isolate)->InternalWriteProperty(Property, Buffer, Value);
}

void FPendingClassConstruction::Finalize(FJavascriptIsolate* Isolate, UObject* UnrealObject)
{
	static_cast<FJavascriptIsolateImplementation*>(Isolate)->RegisterObject(UnrealObject, Object);
	Object->SetAlignedPointerInInternalField(0, UnrealObject);
}

Local<Value> FJavascriptIsolate::ExportStructInstance(Isolate* isolate, UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner)
{
	return FJavascriptIsolateImplementation::GetSelf(isolate)->ExportStructInstance(Struct, Buffer, Owner);
}


template <typename CppType>
bool TStructReader<CppType>::Read(Isolate* isolate, Local<Value> Value, CppType& Target) const
{
	FIsolateHelper I(isolate);

	auto Instance = FStructMemoryInstance::FromV8(Value);
	if (Instance && Instance->Struct == ScriptStruct)
	{
		ScriptStruct->CopyScriptStruct(&Target, Instance->GetMemory());
	}
	else if (Value->IsObject())
	{
		auto v8_v1 = Value->ToObject();

		FJavascriptIsolateImplementation::GetSelf(isolate)->ReadOffStruct(v8_v1, ScriptStruct, reinterpret_cast<uint8*>(&Target));
	}
	else
	{
		I.Throw(TEXT("couldn't read struct"));
		return false;
	}

	return true;
}

namespace v8
{
	Local<Value> ReadProperty(Isolate* isolate, UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner)
	{
		return FJavascriptIsolate::ReadProperty(isolate, Property, Buffer, Owner);
	}

	void WriteProperty(Isolate* isolate, UProperty* Property, uint8* Buffer, Local<Value> Value)
	{
		FJavascriptIsolate::WriteProperty(isolate, Property, Buffer, Value);
	}
}

void FJavascriptFunction::Execute()
{
	if (!Handle.IsValid() || Handle->Function.IsEmpty()) return;

	{
		FPrivateJavascriptFunction* Handle = this->Handle.Get();

		auto isolate_ = Handle->isolate;

		Isolate::Scope isolate_scope(isolate_);
		HandleScope handle_scope(isolate_);

		auto function = Local<Function>::New(Handle->isolate, Handle->Function);
		if (!function.IsEmpty())
		{
			auto context = Local<Context>::New(isolate_, Handle->context);

			Context::Scope context_scope(context);

			function->Call(function, 0, nullptr);
		}
	}
}

void FJavascriptFunction::Execute(UScriptStruct* Struct, void* Buffer)
{
	if (!Handle.IsValid() || Handle->Function.IsEmpty()) return;

	{
		FPrivateJavascriptFunction* Handle = this->Handle.Get();

		auto isolate_ = Handle->isolate;

		Isolate::Scope isolate_scope(isolate_);
		HandleScope handle_scope(isolate_);

		auto function = Local<Function>::New(Handle->isolate, Handle->Function);
		if (!function.IsEmpty())
		{
			auto context = Local<Context>::New(isolate_, Handle->context);

			Context::Scope context_scope(context);

			auto arg = FJavascriptIsolateImplementation::GetSelf(Handle->isolate)->ExportStructInstance(Struct, (uint8*)Buffer, FNoPropertyOwner());
			v8::Handle<Value> args[] = { arg };
			function->Call(function, 1, args);
		}
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS