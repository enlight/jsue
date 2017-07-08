#pragma once

struct FStructMemoryInstance;
class FJavascriptIsolate;

struct FPendingClassConstruction
{
	FPendingClassConstruction() {}
	FPendingClassConstruction(v8::Handle<v8::Object> InObject, UClass* InClass)
		: Object(InObject), Class(InClass)
	{}

	v8::Handle<v8::Object> Object;
	UClass* Class;
	bool bCatched{ false };

	void Finalize(FJavascriptIsolate* Isolate, UObject* Object);
};

class FJavascriptIsolate
{
public:
	/** Maps a UE4 module name to a V8 template of a V8 module (that wraps the UE4 module) */
	TMap<FString, v8::Global<v8::FunctionTemplate>> ModuleNameToFunctionTemplateMap;

	/** A map from Unreal UClass to V8 Function template */
	TMap< UClass*, v8::UniquePersistent<v8::FunctionTemplate> > ClassToFunctionTemplateMap;

	/** A map from Unreal UScriptStruct to V8 Function template */
	TMap< UScriptStruct*, v8::UniquePersistent<v8::FunctionTemplate> > ScriptStructToFunctionTemplateMap;

	/** BlueprintFunctionLibrary function mapping */
	TMultiMap< const UStruct*, UFunction*> BlueprintFunctionLibraryMapping;

	TMultiMap< const UStruct*, UFunction*> BlueprintFunctionLibraryFactoryMapping;

	TArray<FPendingClassConstruction> ObjectUnderConstructionStack;

	v8::Isolate* isolate_;

	static FJavascriptIsolate* Create();
	static v8::Local<v8::Value> ReadProperty(v8::Isolate* isolate, UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner);
	static void WriteProperty(v8::Isolate* isolate, UProperty* Property, uint8* Buffer, v8::Handle<v8::Value> Value);
	static v8::Local<v8::Value> ExportStructInstance(v8::Isolate* isolate, UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner);

	virtual v8::Local<v8::Value> ExportObject(UObject* Object, bool bForce = false) = 0;
	virtual v8::Local<v8::FunctionTemplate> ExportStruct(UScriptStruct* ScriptStruct) = 0;
	virtual v8::Local<v8::FunctionTemplate> ExportClass(UClass* Class, bool bAutoRegister = true) = 0;
	virtual void RegisterClass(UClass* Class, v8::Local<v8::FunctionTemplate> Template) = 0;
	virtual v8::Local<v8::ObjectTemplate> GetGlobalTemplate() = 0;
	virtual void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector) = 0;
	virtual v8::Local<v8::Value> ExportStructInstance(UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner) = 0;
	virtual ~FJavascriptIsolate() {}
};
