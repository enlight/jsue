#include "TypingsGenerator.h"
#include "Object.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Paths.h"
#include "Config.h"
#include "JavascriptIsolate_Private.h"
#include "FileHelper.h"
#include "Kismet/BlueprintFunctionLibrary.h"

struct TokenWriter
{
	TokenWriter(FTypingsGenerator& generator)
		: generator(generator)
	{}

	FTypingsGenerator& generator;

	FString Text;

	void push(const char* something)
	{
		Text.Append(ANSI_TO_TCHAR(something));
	}

	void push(const FString& something)
	{
		Text.Append(something);
	}

	const TCHAR* operator * ()
	{
		return *Text;
	}

	void push(UProperty* Property)
	{
		if (auto p = Cast<UIntProperty>(Property))
		{
			push("number");
		}
		else if (auto p = Cast<UFloatProperty>(Property))
		{
			push("number");
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			push("boolean");
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{
			push("string");
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			push("string");
		}
		else if (auto p = Cast<UTextProperty>(Property))
		{
			push("string");
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			generator.Export(p->MetaClass);

			// @HACK
			push("UnrealEngineClass");
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			generator.Export(p->Struct);
			push(FV8Config::Safeify(p->Struct->GetName()));
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			generator.Export(p->Inner);

			push(p->Inner);
			push("[]");
		}
		else if (auto p = Cast<UByteProperty>(Property))
		{
			if (p->Enum)
			{
				generator.Export(p->Enum);
				push(FV8Config::Safeify(p->Enum->GetName()));
			}
			else
			{
				push("number");
			}
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			generator.Export(p->GetEnum());
			push(FV8Config::Safeify(p->GetEnum()->GetName()));
		}
		else if (auto p = Cast<UMulticastDelegateProperty>(Property))
		{
			push("UnrealEngineMulticastDelegate<");
			push(p->SignatureFunction);
			push(">");
		}
		else if (auto p = Cast<UDelegateProperty>(Property))
		{
			push("UnrealEngineDelegate<");
			push(p->SignatureFunction);
			push(">");
		}
		else if (auto p = Cast<UObjectProperty>(Property))
		{
			generator.Export(p->PropertyClass);
			push(FV8Config::Safeify(p->PropertyClass->GetName()));
		}
		else
		{
			push("any");
		}
	}

	void push(UFunction* SignatureFunction)
	{
		push("(");
		bool first = true;
		TFieldIterator<UProperty> It(SignatureFunction);
		for (; It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
		{
			auto Prop = *It;
			if (!first) push(", ");
			push(FV8Config::Safeify(Prop->GetName()));
			push(": ");
			push(Prop);
			first = false;
		}
		push(") => ");
		bool has_return_value = false;
		for (; It; ++It)
		{
			UProperty* Param = *It;
			if (Param->GetPropertyFlags() & CPF_ReturnParm)
			{
				has_return_value = true;
				push(Param);
				break;
			}
		}
		if (!has_return_value) push("void");
	}

	void tooltip(const char* indent, UField* source)
	{
		if (generator.no_tooltip) return;

		FString Tooltip = source->GetToolTipText().ToString();
		if (Tooltip.Len() > 0)
		{
			TArray<FString> Lines;
			Tooltip.ParseIntoArrayLines(Lines);

			bool first_line = true;
			push(indent);
			push("/**\n");
			for (const auto& line : Lines)
			{
				push(indent);
				push(" * ");
				push(line);
				push("\n");
			}
			push(indent);
			push("*/\n");
		}
	}
};

FTypingsGenerator::FTypingsGenerator(FJavascriptIsolate& InEnvironment)
	: Environment(InEnvironment)
{
}

void FTypingsGenerator::mark_visited(UObject* obj)
{
	visited.Add(obj);
}

bool FTypingsGenerator::has_visited(UObject* obj) const
{
	return (visited.Find(obj) != nullptr);
}

void FTypingsGenerator::Export(UObject* source)
{
	if (has_visited(source)) return;
	mark_visited(source);

	//UE_LOG(Javascript, Log, TEXT("Export %s"), *(source->GetName()));

	if (auto s = Cast<UClass>(source))
	{
		ExportClass(s);
	}
	else if (auto s = Cast<UStruct>(source))
	{
		ExportStruct(s);
	}
	else if (auto s = Cast<UEnum>(source))
	{
		ExportEnum(s);
	}
}

void FTypingsGenerator::fold(bool force)
{
	if (force || Text.Len() > 1024 * 1024)
	{
		Folded.Add(Text);
		Text = TEXT("");
	}
}

void FTypingsGenerator::ExportEnum(UEnum* source)
{
	TokenWriter w(*this);

	FString enumName = FV8Config::Safeify(source->GetName());
	w.push("declare type ");
	w.push(enumName);
	w.push(" = ");

	int32 numMembers = source->NumEnums();
	bool bEnumHasValidMember = false;

	for (int32 i = 0; i < numMembers; ++i)
	{
		FString memberName = source->GetNameStringByIndex(i);
		if (!memberName.IsEmpty())
		{
			if (bEnumHasValidMember && (i > 0))
			{
				w.push(" | ");
			}
			w.push("'");
			w.push(memberName);
			w.push("'");
			bEnumHasValidMember = true;
		}
	}

	w.push(";\n");

	w.push("declare var ");
	w.push(enumName);
	w.push(" : { ");

	bEnumHasValidMember = false;
	for (int32 i = 0; i < numMembers; ++i)
	{
		FString memberName = source->GetNameStringByIndex(i);
		if (!memberName.IsEmpty())
		{
			if (bEnumHasValidMember && (i > 0))
			{
				w.push(",");
			}
			w.push(memberName);
			w.push(":");
			w.push("'");
			w.push(memberName);
			w.push("'");
			bEnumHasValidMember = true;
		}
	}

	w.push(" };\n");

	Text.Append(*w);

	fold();
}

void FTypingsGenerator::ExportClass(UClass* source)
{
	// Skip a generated class
	if (source->ClassGeneratedBy) return;

	auto package = source->GetOutermost();
	auto packageName = package->GetName();
	auto classPath = source->GetPathName();

	ExportStruct(source);
}

void FTypingsGenerator::ExportStruct(UStruct* source)
{
	TokenWriter w(*this);

	const auto name = FV8Config::Safeify(source->GetName());
	auto super_class = source->GetSuperStruct();

	bool bStop = (name == TEXT("PropertyEditor"));

	w.tooltip("", source);

	w.push("declare class ");
	w.push(name);

	if (super_class)
	{
		Export(super_class);

		// The fact that UBlueprintFunctionLibrary is derived from UObject is an implementation
		// detail that's of no relevance to the JS bindings, so omit the relationship from the
		// type definitions to minimize potential naming conflicts in static methods of
		// Blueprint function libs.
		if (Cast<UClass>(source) != UBlueprintFunctionLibrary::StaticClass())
		{
			w.push(" extends ");
			w.push(FV8Config::Safeify(super_class->GetName()));
		}
	}
	w.push(" { \n");

	for (UProperty* property : TFieldRange<UProperty>(source, EFieldIteratorFlags::ExcludeSuper))
	{
		auto propertyName = FV8Config::Safeify(property->GetName());

		w.tooltip("\t", property);

		w.push("\t");
		w.push(propertyName);
		w.push(": ");
		w.push(property);
		w.push(";\n");
	}

	auto write_function = [&](UFunction* Function, bool is_thunk, bool is_factory = false) {
		w.tooltip("\t", Function);

		w.push("\t");
		if (!is_thunk && (Function->FunctionFlags & FUNC_Static))
		{
			w.push("static ");
		}
		w.push(FV8Config::GetAlias(Function, true));
		w.push("(");

		bool has_out_ref = false;
		bool is_optional = false;

		TArray<FString> Arguments;
		for (TFieldIterator<UProperty> ParamIt(Function); ParamIt && (ParamIt->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++ParamIt)
		{
			TokenWriter w2(*this);

			if ((ParamIt->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
			{
				has_out_ref = true;
				is_optional = true;
			}

			auto Property = *ParamIt;
			auto PropertyName = FV8Config::Safeify(Property->GetName());

			w2.push(PropertyName);
			if (is_optional)
			{
				w2.push("?");
			}
			w2.push(": ");
			w2.push(Property);

			Arguments.Add(*w2);
		}

		if (is_thunk)
		{
			Arguments.RemoveAt(0);
		}

		w.push(FString::Join(Arguments, TEXT(",")));
		w.push("): ");

		if (has_out_ref)
		{
			TArray<FString> Arguments;
			for (UProperty* param : TFieldRange<UProperty>(Function))
			{
				TokenWriter w2(*this);

				if (param->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm))
				{
					w2.push("$: ");
					w2.push(param);

					Arguments.Add(*w2);
				}
				else if ((param->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					w2.push(param->GetName());
					w2.push(": ");
					w2.push(param);

					Arguments.Add(*w2);
				}
			}
			w.push("{");
			w.push(FString::Join(Arguments, TEXT(", ")));
			w.push("}");
		}
		else
		{
			bool has_return = false;
			for (UProperty* param : TFieldRange<UProperty>(Function))
			{
				if (param->HasAllPropertyFlags(CPF_Parm | CPF_ReturnParm))
				{
					w.push(param);
					has_return = true;
					break;
				}
			}

			if (!has_return)
			{
				w.push("void");
			}
		}


		w.push(";\n");
	};

	if (auto klass = Cast<UClass>(source))
	{

		bool bIsUObject = (klass == UObject::StaticClass() || !klass->IsChildOf(UObject::StaticClass()));

		if (klass->IsChildOf(AActor::StaticClass()))
		{
			Export(UWorld::StaticClass());
			if (klass == AActor::StaticClass()) {
				w.push("\tconstructor(InWorld: World, Location?: Vector, Rotation?: Rotator);\n");
			}
		}
		else
		{
			if (bIsUObject) {
				w.push("\tconstructor();\n");
				w.push("\tconstructor(Outer: UObject);\n");
			}
			w.push("\tstatic Load(ResourceName: string): ");
			w.push(name);
			w.push(";\n");
			w.push("\tstatic Find(Outer: UObject, ResourceName: string): ");
			w.push(name);
			w.push(";\n");
		}

		if (bIsUObject) {
			w.push("\tstatic StaticClass: any;\n");

			w.push("\tstatic GetClassObject(): Class;\n");
		}

		w.push("\tstatic GetDefaultObject(): ");
		w.push(name);
		w.push(";\n");

		if (bIsUObject) {
			w.push("\tstatic GetDefaultSubobjectByName(Name: string): UObject;\n");
			w.push("\tstatic SetDefaultSubobjectClass(Name: string): void;\n");
		}
		w.push("\tstatic CreateDefaultSubobject(Name: string, Transient?: boolean, Required?: boolean, Abstract?: boolean): ");
		w.push(name);
		w.push(";\n");

		for (UFunction* function : TFieldRange<UFunction>(klass, EFieldIteratorFlags::ExcludeSuper))
		{
			if (FV8Config::CanExportFunction(klass, function))
			{
				write_function(function, false);
			}
		}
	}
	else
	{
		w.push("\tclone() : ");
		w.push(name);
		w.push(";\n");
	}

	{
		w.push("\tstatic C(Other: UObject): ");
		w.push(name);
		w.push(";\n");

		TArray<UFunction*> Functions;
		Environment.BlueprintFunctionLibraryMapping.MultiFind(source, Functions);

		for (auto Function : Functions)
		{
			write_function(Function, true);
		}

		Environment.BlueprintFunctionLibraryFactoryMapping.MultiFind(source, Functions);
		for (auto Function : Functions)
		{
			write_function(Function, false, true);
		}
	}

	w.push("}\n\n");

	Text.Append(*w);

	fold();
}

void FTypingsGenerator::ExportBootstrap()
{
	TokenWriter w(*this);
	w.push("declare function gc() : void;\n");
	w.push("declare type UnrealEngineClass = any;\n");

	w.push("declare type timeout_handle = any;\n");
	w.push("declare function setTimeout(fn : (milliseconds: number) => void, timeout : number) : timeout_handle;\n");
	w.push("declare function clearTimeout(handle : timeout_handle) : void;\n");

	w.push("declare class UnrealEngineMulticastDelegate<T> {\n");
	w.push("\tAdd(fn : T): void;\n");
	w.push("\tRemove(fn : T): void;\n");
	w.push("}\n\n");

	w.push("declare class UnrealEngineDelegate<T> {\n");
	w.push("\tAdd(fn : T): void;\n");
	w.push("\tRemove(fn : T): void;\n");
	w.push("}\n\n");

	w.push("declare class Process {\n");
	w.push("\tnextTick(fn : (elapsedTimeInMillisecs: number) => void): void;\n");
	w.push("}\n\n");
	w.push("declare var process : Process;\n\n");

	w.push("declare class Memory {\n");
	w.push("\texec(ab : ArrayBuffer, fn : (ab : ArrayBuffer) => void): void;\n");
	w.push("\taccess(obj : JavascriptMemoryObject): ArrayBuffer;\n");
	w.push("}\n\n");
	w.push("declare var memory : Memory;\n\n");

	Text.Append(*w);
}

void FTypingsGenerator::ExportWKO(FString name, UObject* Object)
{
	TokenWriter w(*this);

	Export(Object->GetClass());

	w.push("declare var ");
	w.push(name);
	w.push(" : ");
	w.push(FV8Config::Safeify(Object->GetClass()->GetName()));

	w.push(";\n\n");

	Text.Append(*w);
}

void FTypingsGenerator::Finalize()
{
	fold(true);
}

bool FTypingsGenerator::Save(const FString& Filename)
{
	FString Path, BaseFilename, Extension;

	FPaths::Split(Filename, Path, BaseFilename, Extension);

	for (int32 Index = 0; Index < Folded.Num(); ++Index)
	{
		const bool is_last = (Index == (Folded.Num() - 1));

		FString Text = Folded[Index];

		auto page_name = [&](int32 Index) {
			return FString::Printf(TEXT("_part_%d_%s.%s"), Index, *BaseFilename, *Extension);
		};

		FString PageFilename = is_last ? Filename : FPaths::Combine(*Path, *page_name(Index));

		if (is_last)
		{
			FString Header;

			for (int32 Prev = 0; Prev < Index; ++Prev)
			{
				Header.Append(FString::Printf(TEXT("/// <reference path=\"%s\">/>\n"), *page_name(Prev)));
			}

			Header.Append(Text);
			Text = Header;
		}

		if (!FFileHelper::SaveStringToFile(Text, *PageFilename)) return false;
	}

	return true;
}