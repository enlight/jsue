#pragma once

class FJavascriptIsolate;

class FTypingsGenerator
{
public:
	bool no_tooltip{ false };

public:
	FTypingsGenerator(FJavascriptIsolate& InEnvironment);
	void Export(UObject* source);
	void ExportBootstrap();
	void ExportWKO(FString name, UObject* Object);
	void Finalize();
	bool Save(const FString& Filename);

private:
	TSet<UObject*> visited;
	FJavascriptIsolate& Environment;
	FString Text;
	TArray<FString> Folded;
	
private:
	void mark_visited(UObject* obj);
	bool has_visited(UObject* obj) const;
	void fold(bool force = false);
	void ExportClass(UClass* source);
	void ExportStruct(UStruct* source);
	void ExportEnum(UEnum* source);
};
