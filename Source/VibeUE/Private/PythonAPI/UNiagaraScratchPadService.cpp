// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "PythonAPI/UNiagaraScratchPadService.h"

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/Guid.h"

// Niagara runtime
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

// Niagara data interfaces (for typed pins / module inputs)
#include "NiagaraDataInterfaceArrayFloat.h"
#include "NiagaraDataInterfaceArrayInt.h"
#include "NiagaraDataInterfaceGrid2DCollection.h"
#include "NiagaraDataInterfaceRenderTarget2D.h"

// Niagara editor.
// NOTE: MapGet, MapSet, and If node headers live in NiagaraEditor's Private/ folder,
// so we cannot include them from a dependent plugin. We instead operate on those nodes
// through the public UNiagaraNodeWithDynamicPins base class and instantiate them via
// runtime class lookup (FindObject<UClass> on /Script/NiagaraEditor.NiagaraNode...).
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraScriptSource.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#define LOCTEXT_NAMESPACE "NiagaraScratchPadService"

// =====================================================================
// File-static helpers
// =====================================================================

namespace
{
	UNiagaraSystem* LoadSystem(const FString& SystemPath)
	{
		if (SystemPath.IsEmpty())
		{
			return nullptr;
		}
		UObject* Loaded = UEditorAssetLibrary::LoadAsset(SystemPath);
		return Cast<UNiagaraSystem>(Loaded);
	}

	FNiagaraEmitterHandle* FindEmitterHandle(UNiagaraSystem* System, const FString& EmitterName)
	{
		if (!System)
		{
			return nullptr;
		}
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
			{
				return &Handle;
			}
		}
		return nullptr;
	}

	ENiagaraScriptUsage StageToUsage(const FString& Stage)
	{
		if (Stage.Equals(TEXT("ParticleSpawn"),  ESearchCase::IgnoreCase)) return ENiagaraScriptUsage::ParticleSpawnScript;
		if (Stage.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase)) return ENiagaraScriptUsage::ParticleUpdateScript;
		if (Stage.Equals(TEXT("EmitterSpawn"),   ESearchCase::IgnoreCase)) return ENiagaraScriptUsage::EmitterSpawnScript;
		if (Stage.Equals(TEXT("EmitterUpdate"),  ESearchCase::IgnoreCase)) return ENiagaraScriptUsage::EmitterUpdateScript;
		// Default to particle update - matches the prior service's default
		return ENiagaraScriptUsage::ParticleUpdateScript;
	}

	UNiagaraScript* GetStageScript(FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage)
	{
		if (!EmitterData) return nullptr;
		switch (Usage)
		{
			case ENiagaraScriptUsage::ParticleSpawnScript:  return EmitterData->SpawnScriptProps.Script;
			case ENiagaraScriptUsage::ParticleUpdateScript: return EmitterData->UpdateScriptProps.Script;
#if WITH_EDITORONLY_DATA
			case ENiagaraScriptUsage::EmitterSpawnScript:   return EmitterData->EmitterSpawnScriptProps.Script;
			case ENiagaraScriptUsage::EmitterUpdateScript:  return EmitterData->EmitterUpdateScriptProps.Script;
#endif
			default: return EmitterData->UpdateScriptProps.Script;
		}
	}

	UNiagaraGraph* GetEmitterGraph(FVersionedNiagaraEmitterData* EmitterData)
	{
		if (!EmitterData) return nullptr;
		if (auto* Src = Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
		{
			return Src->NodeGraph;
		}
		return nullptr;
	}

	/** Pick a name for a new scratch script that is unique within its outer. */
	FName GetUniqueScriptName(UObject* Outer, const TCHAR* Base)
	{
		FName Candidate(Base);
		int32 N = 0;
		while (FindObjectFast<UNiagaraScript>(Outer, Candidate) != nullptr)
		{
			++N;
			Candidate = FName(*FString::Printf(TEXT("%s_%d"), Base, N));
		}
		return Candidate;
	}

	/** Resolve a "Module.Foo" or "Output.X" namespaced name; bare names get a sensible namespace. */
	FString NormalizeNamespacedName(const FString& InName, const FString& DefaultNamespace)
	{
		if (InName.Contains(TEXT(".")))
		{
			return InName;
		}
		if (DefaultNamespace.IsEmpty())
		{
			return InName;
		}
		return DefaultNamespace + TEXT(".") + InName;
	}

	bool ResolveType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
	{
		const FString N = TypeName.TrimStartAndEnd();
		if (N.IsEmpty()) return false;

		// Scalars / vectors / common
		if (N.Equals(TEXT("float"),  ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetFloatDef();    return true; }
		if (N.Equals(TEXT("int"),    ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("int32"),  ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetIntDef();      return true; }
		if (N.Equals(TEXT("bool"),   ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetBoolDef();     return true; }
		if (N.Equals(TEXT("vec2"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("vector2"),ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetVec2Def();     return true; }
		if (N.Equals(TEXT("vec3"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("vector"), ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("vector3"),ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetVec3Def();     return true; }
		if (N.Equals(TEXT("vec4"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("vector4"),ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetVec4Def();     return true; }
		if (N.Equals(TEXT("position"),ESearchCase::IgnoreCase)){ OutType = FNiagaraTypeDefinition::GetPositionDef(); return true; }
		if (N.Equals(TEXT("color"),  ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition::GetColorDef(); return true; }

		// Data-interface array types (used to pass arrays from Blueprints into Niagara)
		if (N.Equals(TEXT("ArrayFloat"),    ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat::StaticClass());    return true; }
		if (N.Equals(TEXT("ArrayFloat2"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("ArrayVec2"),     ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat2::StaticClass());   return true; }
		if (N.Equals(TEXT("ArrayVector"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("ArrayFloat3"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("ArrayVec3"),     ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass());   return true; }
		if (N.Equals(TEXT("ArrayFloat4"),   ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("ArrayVec4"),     ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat4::StaticClass());   return true; }
		if (N.Equals(TEXT("ArrayPosition"), ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayPosition::StaticClass()); return true; }
		if (N.Equals(TEXT("ArrayInt"),      ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("ArrayInt32"),    ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayInt32::StaticClass());    return true; }
		if (N.Equals(TEXT("Grid2D"),        ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("Grid2DCollection"), ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceGrid2DCollection::StaticClass()); return true; }
		if (N.Equals(TEXT("RenderTarget2D"), ESearchCase::IgnoreCase) ||
		    N.Equals(TEXT("DynamicRT"),      ESearchCase::IgnoreCase)) { OutType = FNiagaraTypeDefinition(UNiagaraDataInterfaceRenderTarget2D::StaticClass()); return true; }

		return false;
	}

	/** Look up a NiagaraEditor node class by its short name. The result is cached per call site. */
	UClass* GetNiagaraEditorClass(const TCHAR* ShortName)
	{
		const FString FullPath = FString::Printf(TEXT("/Script/NiagaraEditor.%s"), ShortName);
		return FindObject<UClass>(nullptr, *FullPath);
	}

	UClass* GetMapGetClass()  { static UClass* C = GetNiagaraEditorClass(TEXT("NiagaraNodeParameterMapGet")); return C; }
	UClass* GetMapSetClass()  { static UClass* C = GetNiagaraEditorClass(TEXT("NiagaraNodeParameterMapSet")); return C; }
	UClass* GetIfClass()      { static UClass* C = GetNiagaraEditorClass(TEXT("NiagaraNodeIf"));              return C; }

	bool IsMapGet(const UEdGraphNode* N) { UClass* C = GetMapGetClass(); return N && C && N->IsA(C); }
	bool IsMapSet(const UEdGraphNode* N) { UClass* C = GetMapSetClass(); return N && C && N->IsA(C); }
	bool IsIfNode(const UEdGraphNode* N) { UClass* C = GetIfClass();     return N && C && N->IsA(C); }

	FString GetNodeTypeLabel(UEdGraphNode* Node)
	{
		if (!Node) return TEXT("");
		if (Node->IsA<UNiagaraNodeCustomHlsl>())        return TEXT("CustomHlsl");
		if (IsMapGet(Node))                             return TEXT("MapGet");
		if (IsMapSet(Node))                             return TEXT("MapSet");
		if (Node->IsA<UNiagaraNodeOp>())                return TEXT("Op");
		if (IsIfNode(Node))                             return TEXT("If");
		if (Node->IsA<UNiagaraNodeInput>())             return TEXT("Input");
		if (Node->IsA<UNiagaraNodeOutput>())            return TEXT("Output");
		if (Node->IsA<UNiagaraNodeFunctionCall>())      return TEXT("FunctionCall");
		return Node->GetClass()->GetName();
	}

	// ---- Cross-module workarounds for non-exported NiagaraEditor APIs ----
	// UNiagaraNodeCustomHlsl::Set/GetCustomHlsl and UNiagaraNodeWithDynamicPins::AddParameterPin /
	// RequestNewTypedPin are declared in public headers but NOT marked NIAGARAEDITOR_API, so the
	// symbols are not exported from the engine DLL. We reach the same behavior via:
	//   * FProperty reflection on the UPROPERTY-backed `CustomHlsl` field, plus PostEditChangeProperty
	//     to trigger the node's internal RebuildSignatureFromPins.
	//   * UEdGraphSchema_Niagara::TypeDefinitionToPinType (NIAGARAEDITOR_API) + UEdGraphNode::CreatePin
	//     (Engine module, exported) for pin creation. Pin names for parameter-map nodes are the
	//     fully-namespaced variable names, which the Niagara compiler reads directly.

	FProperty* FindCustomHlslProperty(UEdGraphNode* Node)
	{
		return Node ? Node->GetClass()->FindPropertyByName(TEXT("CustomHlsl")) : nullptr;
	}

	void SetCustomHlslReflected(UEdGraphNode* HlslNode, const FString& Code)
	{
		if (!HlslNode) return;
		FProperty* Prop = FindCustomHlslProperty(HlslNode);
		if (auto* StrProp = CastField<FStrProperty>(Prop))
		{
			HlslNode->Modify();
			StrProp->SetPropertyValue_InContainer(HlslNode, Code);
			FPropertyChangedEvent Evt(Prop, EPropertyChangeType::ValueSet);
			HlslNode->PostEditChangeProperty(Evt);
		}
	}

	FString GetCustomHlslReflected(UEdGraphNode* HlslNode)
	{
		FProperty* Prop = FindCustomHlslProperty(HlslNode);
		if (auto* StrProp = CastField<FStrProperty>(Prop))
		{
			return StrProp->GetPropertyValue_InContainer(HlslNode);
		}
		return FString();
	}

	/** UNiagaraNodeWithDynamicPins::IsAddPin is unexported; same check (Misc category, "DynamicAddPin"). */
	bool IsDynamicAddPin(const UEdGraphPin* Pin)
	{
		static const FName AddPinSubCategory(TEXT("DynamicAddPin"));
		return Pin
			&& Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
			&& Pin->PinType.PinSubCategory == AddPinSubCategory;
	}

	/**
	 * UEdGraphNode::CreatePin appends, so a pin made on a dynamic-pin node lands AFTER that side's
	 * "+" add pin. The engine's own RequestNewTypedPin inserts before it, and the stack view relies on
	 * that: UNiagaraNodeCustomHlsl::BuildParameterMapHistory pairs pin i with Signature.Inputs[i] and
	 * only skips the add pin, so a real pin behind the add pin reads one past the end of the signature
	 * ("Array index out of bounds: 4 into an array of size 4" with four inputs). The compiler takes a
	 * different path and stays green, which is why the crash only shows when a stack view is built
	 * (opening the system, or GetScriptStackTopology). Move the new pin in front of the add pin.
	 */
	void PlacePinBeforeAddPin(UEdGraphNode* Node, UEdGraphPin* NewPin)
	{
		if (!Node || !NewPin) return;
		const int32 NewIndex = Node->Pins.Find(NewPin);
		if (NewIndex == INDEX_NONE) return;
		for (int32 i = 0; i < NewIndex; ++i)
		{
			const UEdGraphPin* Pin = Node->Pins[i];
			if (IsDynamicAddPin(Pin) && Pin->Direction == NewPin->Direction)
			{
				Node->Pins.RemoveAt(NewIndex);
				Node->Pins.Insert(NewPin, i);
				return;
			}
		}
	}

	/** True when, on each side, nothing but other add pins follows the first add pin. */
	bool HasAddPinsLast(const UEdGraphNode* Node)
	{
		bool bSeenAdd[2] = { false, false };
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			const int32 Side = Pin->Direction == EGPD_Input ? 0 : 1;
			if (IsDynamicAddPin(Pin))
			{
				bSeenAdd[Side] = true;
			}
			else if (bSeenAdd[Side])
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Move every add pin behind the real pins, keeping the real pins' relative order. Pin order
	 * within a side is all that changes, so links, names and the compiled result stay the same.
	 * Returns true when the order changed.
	 */
	bool MoveAddPinsLast(UEdGraphNode* Node)
	{
		if (!Node || HasAddPinsLast(Node)) return false;
		Node->Modify();
		TArray<UEdGraphPin*> Real;
		TArray<UEdGraphPin*> Add;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			(IsDynamicAddPin(Pin) ? Add : Real).Add(Pin);
		}
		Real.Append(Add);
		Node->Pins = MoveTemp(Real);
		return true;
	}

	/**
	 * Replicate UNiagaraNodeCustomHlsl::RebuildSignatureFromPins (which is protected and not
	 * exported across modules). We rebuild the function signature from the current pin set
	 * using the exported Schema->PinToNiagaraVariable helper and the public Signature UPROPERTY
	 * on UNiagaraNodeFunctionCall. This is the ONLY way to make a freshly-added Custom HLSL
	 * pin survive a subsequent compile: the alternative (triggering PostEditChangeProperty)
	 * runs RefreshFromExternalChanges → ReallocatePins(false), which rebuilds pins from the
	 * stale signature and silently wipes any pin that isn't in it yet.
	 *
	 * Signature order follows pin order, so call PlacePinBeforeAddPin first: the stack view pairs
	 * pins and signature entries by index.
	 */
	void RebuildCustomHlslSignatureFromPins(UEdGraphNode* HlslNode)
	{
		auto* FnCall = Cast<UNiagaraNodeFunctionCall>(HlslNode);
		if (!FnCall) return;
		const UEdGraphSchema_Niagara* Schema = Cast<UEdGraphSchema_Niagara>(HlslNode->GetSchema());
		if (!Schema) return;

		HlslNode->Modify();
		FNiagaraFunctionSignature& Sig = FnCall->Signature;
		Sig.Inputs.Empty();
		Sig.Outputs.Empty();

		for (UEdGraphPin* Pin : HlslNode->Pins)
		{
			if (!Pin) continue;
			// Skip "Add" placeholder pins (the "+" buttons on each side of the node).
			if (IsDynamicAddPin(Pin))
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input)
			{
				Sig.Inputs.Add(Schema->PinToNiagaraVariable(Pin, /*bNeedsValue*/true));
			}
			else
			{
				Sig.Outputs.Add(Schema->PinToNiagaraVariable(Pin, /*bNeedsValue*/false));
			}
		}
	}

	/** Build an FEdGraphPinType for a Niagara type using the exported schema helper. */
	FEdGraphPinType MakeNiagaraPinType(const FNiagaraTypeDefinition& Type)
	{
		return UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type);
	}

	/**
	 * Instantiate a NiagaraEditor node by its UClass and add it to the graph, mirroring what
	 * FGraphNodeCreator<T>::Finalize() does. Used when the concrete class header is private.
	 */
	UEdGraphNode* SpawnNodeByClass(UNiagaraGraph* Graph, UClass* NodeClass, int32 PosX, int32 PosY)
	{
		if (!Graph || !NodeClass) return nullptr;
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
		if (!Node) return nullptr;
		Graph->AddNode(Node, /*bFromUI*/false, /*bSelectNewNode*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Node->NodePosX = PosX;
		Node->NodePosY = PosY;
		return Node;
	}

	UEdGraphNode* FindNodeByGuid(UNiagaraGraph* Graph, const FString& NodeId)
	{
		if (!Graph) return nullptr;
		FGuid Guid;
		if (!FGuid::Parse(NodeId, Guid)) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid) return Node;
		}
		return nullptr;
	}

	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction, bool bConstrainDirection)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (bConstrainDirection && Pin->Direction != Direction) continue;
			if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) ||
			    Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Find the function-call (stack module) node by display name across all stage graphs of an emitter. */
	UNiagaraNodeFunctionCall* FindModuleNodeOnEmitter(FVersionedNiagaraEmitterData* EmitterData, const FString& ModuleName)
	{
		UNiagaraGraph* Graph = GetEmitterGraph(EmitterData);
		if (!Graph) return nullptr;
		TArray<UNiagaraNodeFunctionCall*> Calls;
		Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);
		for (UNiagaraNodeFunctionCall* Call : Calls)
		{
			if (!Call) continue;
			if (Call->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
			{
				return Call;
			}
			if (Call->FunctionScript && Call->FunctionScript->GetName().Equals(ModuleName, ESearchCase::IgnoreCase))
			{
				return Call;
			}
		}
		return nullptr;
	}

	UNiagaraGraph* GetScratchGraphFromCall(UNiagaraNodeFunctionCall* Call)
	{
		if (!Call || !Call->FunctionScript) return nullptr;
		auto* Src = Cast<UNiagaraScriptSource>(Call->FunctionScript->GetLatestSource());
		return Src ? Src->NodeGraph : nullptr;
	}

	/** Walk an emitter -> its scratch-pad scripts container, return the container even when null-on-disk. */
	UNiagaraScratchPadContainer* GetOrCreateScratchContainer(FNiagaraEmitterHandle* Handle, FVersionedNiagaraEmitterData* EmitterData)
	{
		if (!Handle || !EmitterData) return nullptr;
		if (EmitterData->ScratchPads)
		{
			return EmitterData->ScratchPads;
		}
		UNiagaraEmitter* OuterEmitter = Handle->GetInstance().Emitter;
		if (!OuterEmitter) return nullptr;
		UNiagaraScratchPadContainer* Container = NewObject<UNiagaraScratchPadContainer>(OuterEmitter, NAME_None, RF_Transactional);
		EmitterData->ScratchPads = Container;
		return Container;
	}

	UNiagaraNodeOutput* FindStageOutputNode(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, UNiagaraScript* StageScript)
	{
		if (!Graph) return nullptr;
		UNiagaraNodeOutput* Out = Graph->FindEquivalentOutputNode(Usage, StageScript ? StageScript->GetUsageId() : FGuid());
		if (Out) return Out;
		TArray<UNiagaraNodeOutput*> All;
		Graph->GetNodesOfClass<UNiagaraNodeOutput>(All);
		for (UNiagaraNodeOutput* O : All)
		{
			if (O && O->GetUsage() == Usage) return O;
		}
		return nullptr;
	}
} // anonymous namespace

// =====================================================================
// Lifecycle
// =====================================================================

FNiagaraScratchResult UNiagaraScratchPadService::CreateScratchModule(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& Stage,
	const FString& DesiredName)
{
	FNiagaraScratchResult Result;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { Result.Message = TEXT("System not found"); return Result; }

	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { Result.Message = TEXT("Emitter not found"); return Result; }

	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
	if (!EmitterData) { Result.Message = TEXT("Emitter has no editable data"); return Result; }

	UNiagaraScratchPadContainer* Container = GetOrCreateScratchContainer(Handle, EmitterData);
	if (!Container) { Result.Message = TEXT("Could not access scratch-pad container"); return Result; }

	const ENiagaraScriptUsage TargetUsage = StageToUsage(Stage);
	UNiagaraScript* StageScript = GetStageScript(EmitterData, TargetUsage);
	UNiagaraGraph* EmitterGraph = GetEmitterGraph(EmitterData);
	if (!StageScript || !EmitterGraph) { Result.Message = TEXT("Emitter stage graph unavailable"); return Result; }

	UNiagaraNodeOutput* OutputNode = FindStageOutputNode(EmitterGraph, TargetUsage, StageScript);
	if (!OutputNode) { Result.Message = TEXT("No output node for the target stage"); return Result; }

	// Build the scratch UNiagaraScript by duplicating the engine's default module template,
	// matching what UNiagaraScratchPadViewModel::CreateNewScript does for Module-usage scripts.
	UObject* ScriptOuter = Container;
	if (!ScriptOuter->HasAnyFlags(RF_Transactional))
	{
		ScriptOuter->SetFlags(RF_Transactional);
	}
	ScriptOuter->Modify();

	const TCHAR* BaseName = DesiredName.IsEmpty() ? TEXT("ScratchModule") : *DesiredName;
	const FName UniqueName = GetUniqueScriptName(ScriptOuter, BaseName);

	// Duplicate the engine's DefaultModuleScript template (this is exactly what
	// UNiagaraScratchPadViewModel::CreateNewScript does for Module-usage scripts). We do not
	// fall back to UNiagaraScriptFactoryNew::InitializeScript: it is not exported across
	// modules in stock 5.7. The default template is always present in Niagara-enabled projects.
	UNiagaraScript* DefaultModule = Cast<UNiagaraScript>(GetDefault<UNiagaraEditorSettings>()->DefaultModuleScript.TryLoad());
	if (!DefaultModule
		|| Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource()) == nullptr
		|| Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource())->NodeGraph == nullptr)
	{
		Result.Message = TEXT("UNiagaraEditorSettings::DefaultModuleScript is missing or invalid - cannot create scratch module");
		return Result;
	}
	UNiagaraScript* NewScript = CastChecked<UNiagaraScript>(StaticDuplicateObject(DefaultModule, ScriptOuter, UniqueName));

	if (!NewScript) { Result.Message = TEXT("Failed to duplicate default module script"); return Result; }

	NewScript->ClearFlags(RF_Public | RF_Standalone);
	Container->Scripts.Add(NewScript);

	// Allow the script to live on the target stage's stack
	if (FVersionedNiagaraScriptData* ScriptData = NewScript->GetLatestScriptData())
	{
		ScriptData->ModuleUsageBitmask |= (1 << (int32)TargetUsage);
	}

	// Insert as a stack module at the end of the stage
	EmitterGraph->Modify();
	UNiagaraNodeFunctionCall* ModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		NewScript, *OutputNode, INDEX_NONE, BaseName, FGuid());

	if (!ModuleNode)
	{
		// Rollback - leave the script in the container but report failure cleanly
		Result.Message = FString::Printf(TEXT("AddScriptModuleToStack returned null for %s/%s"), *EmitterName, *Stage);
		return Result;
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);
	System->WaitForCompilationComplete();

	Result.bSuccess  = true;
	Result.Message   = TEXT("Scratch module created");
	Result.ModuleName = ModuleNode->GetFunctionName();
	Result.ScriptPath = NewScript->GetPathName();
	Result.NodeId    = ModuleNode->NodeGuid.ToString();
	return Result;
}

FString UNiagaraScratchPadService::GetScratchScriptPath(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FString();
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return FString();
	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(EmitterData, ModuleName);
	if (!Call || !Call->FunctionScript) return FString();
	// Confirm it's a scratch script (i.e. transient outer = scratch container or system) vs. asset
	return Call->FunctionScript->GetPathName();
}

TArray<FString> UNiagaraScratchPadService::ListScratchModules(
	const FString& SystemPath,
	const FString& EmitterName)
{
	TArray<FString> Out;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return Out;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return Out;
	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
	if (!EmitterData || !EmitterData->ScratchPads) return Out;

	UNiagaraGraph* Graph = GetEmitterGraph(EmitterData);
	if (!Graph) return Out;

	TSet<UNiagaraScript*> ScratchScripts(EmitterData->ScratchPads->Scripts);

	TArray<UNiagaraNodeFunctionCall*> Calls;
	Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);
	for (UNiagaraNodeFunctionCall* Call : Calls)
	{
		if (Call && Call->FunctionScript && ScratchScripts.Contains(Call->FunctionScript))
		{
			Out.Add(Call->GetFunctionName());
		}
	}
	return Out;
}

TArray<FString> UNiagaraScratchPadService::ListEmitters(const FString& SystemPath)
{
	TArray<FString> Names;
	if (UNiagaraSystem* System = LoadSystem(SystemPath))
	{
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			Names.Add(Handle.GetName().ToString());
		}
	}
	return Names;
}

// =====================================================================
// Inspection
// =====================================================================

TArray<FNiagaraScratchNodeInfo> UNiagaraScratchPadService::ListNodes(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName)
{
	TArray<FNiagaraScratchNodeInfo> Out;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return Out;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return Out;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) return Out;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		FNiagaraScratchNodeInfo Info;
		Info.NodeId    = Node->NodeGuid.ToString();
		Info.NodeType  = GetNodeTypeLabel(Node);
		Info.Title     = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Info.PosX      = Node->NodePosX;
		Info.PosY      = Node->NodePosY;
		// UNiagaraNode exposes IsNodeEnabled but the base UEdGraphNode has EnabledState; both work.
		Info.bIsEnabled = (Node->GetDesiredEnabledState() == ENodeEnabledState::Enabled);
		Out.Add(Info);
	}
	return Out;
}

TArray<FNiagaraScratchPinInfo> UNiagaraScratchPadService::GetNodePins(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId)
{
	TArray<FNiagaraScratchPinInfo> Out;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return Out;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return Out;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node  = FindNodeByGuid(Graph, NodeId);
	if (!Node) return Out;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		FNiagaraScratchPinInfo Info;
		Info.PinName     = Pin->PinName.ToString();
		Info.Direction   = (Pin->Direction == EGPD_Input) ? TEXT("Input") : TEXT("Output");
		Info.TypeName    = Pin->PinType.PinSubCategoryObject.IsValid()
		                       ? Pin->PinType.PinSubCategoryObject->GetName()
		                       : Pin->PinType.PinCategory.ToString();
		Info.bIsConnected = Pin->LinkedTo.Num() > 0;
		Info.DefaultValue = Pin->DefaultValue;
		Info.PinId       = Pin->PinId.ToString();
		Out.Add(Info);
	}
	return Out;
}

TArray<FNiagaraScratchConnectionInfo> UNiagaraScratchPadService::ListConnections(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName)
{
	TArray<FNiagaraScratchConnectionInfo> Out;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return Out;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return Out;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) return Out;

	// Each link is reported once (from the OUTPUT side).
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode()) continue;
				FNiagaraScratchConnectionInfo C;
				C.FromNodeId = Node->NodeGuid.ToString();
				C.FromPin    = Pin->PinName.ToString();
				C.ToNodeId   = Linked->GetOwningNode()->NodeGuid.ToString();
				C.ToPin      = Linked->PinName.ToString();
				Out.Add(C);
			}
		}
	}
	return Out;
}

// =====================================================================
// Node authoring
// =====================================================================

FNiagaraScratchResult UNiagaraScratchPadService::AddNode(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeType,
	int32 PosX,
	int32 PosY)
{
	FNiagaraScratchResult R;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { R.Message = TEXT("System not found"); return R; }
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { R.Message = TEXT("Emitter not found"); return R; }
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) { R.Message = TEXT("Scratch module not found"); return R; }

	Graph->Modify();

	UEdGraphNode* New = nullptr;
	if (NodeType.Equals(TEXT("MapGet"), ESearchCase::IgnoreCase))
	{
		UClass* C = GetMapGetClass();
		if (!C) { R.Message = TEXT("NiagaraNodeParameterMapGet class not loaded"); return R; }
		New = SpawnNodeByClass(Graph, C, PosX, PosY);
	}
	else if (NodeType.Equals(TEXT("MapSet"), ESearchCase::IgnoreCase))
	{
		UClass* C = GetMapSetClass();
		if (!C) { R.Message = TEXT("NiagaraNodeParameterMapSet class not loaded"); return R; }
		New = SpawnNodeByClass(Graph, C, PosX, PosY);
	}
	else if (NodeType.Equals(TEXT("If"), ESearchCase::IgnoreCase))
	{
		UClass* C = GetIfClass();
		if (!C) { R.Message = TEXT("NiagaraNodeIf class not loaded"); return R; }
		New = SpawnNodeByClass(Graph, C, PosX, PosY);
	}
	else if (NodeType.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UNiagaraNodeInput> Creator(*Graph);
		UNiagaraNodeInput* N = Creator.CreateNode();
		N->NodePosX = PosX; N->NodePosY = PosY;
		// Caller can use add_pin with a typed parameter; leave Input.Variable unset for now.
		Creator.Finalize();
		New = N;
	}
	else
	{
		R.Message = FString::Printf(TEXT("Unknown NodeType '%s' - use MapGet, MapSet, If, Input, or call add_op_node / add_custom_hlsl_node"), *NodeType);
		return R;
	}

	if (!New) { R.Message = TEXT("Node creation returned null"); return R; }
	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();

	R.bSuccess = true;
	R.Message  = TEXT("Node added");
	R.NodeId   = New->NodeGuid.ToString();
	return R;
}

FNiagaraScratchResult UNiagaraScratchPadService::AddOpNode(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& OpName,
	int32 PosX,
	int32 PosY)
{
	FNiagaraScratchResult R;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { R.Message = TEXT("System not found"); return R; }
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { R.Message = TEXT("Emitter not found"); return R; }
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) { R.Message = TEXT("Scratch module not found"); return R; }

	Graph->Modify();
	FGraphNodeCreator<UNiagaraNodeOp> Creator(*Graph);
	UNiagaraNodeOp* N = Creator.CreateNode();

	// Accept either full "Namespace::Op" or a bare op name (try Numeric:: prefix).
	const FName Direct(*OpName);
	const FName Namespaced(*FString::Printf(TEXT("Numeric::%s"), *OpName));
	N->OpName = OpName.Contains(TEXT("::")) ? Direct : Namespaced;
	N->NodePosX = PosX; N->NodePosY = PosY;
	Creator.Finalize();

	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();

	R.bSuccess = true;
	R.Message  = FString::Printf(TEXT("Op node added (%s)"), *N->OpName.ToString());
	R.NodeId   = N->NodeGuid.ToString();
	return R;
}

FNiagaraScratchResult UNiagaraScratchPadService::AddCustomHlslNode(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& Code,
	int32 PosX,
	int32 PosY)
{
	FNiagaraScratchResult R;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { R.Message = TEXT("System not found"); return R; }
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { R.Message = TEXT("Emitter not found"); return R; }
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) { R.Message = TEXT("Scratch module not found"); return R; }

	Graph->Modify();
	FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(*Graph);
	UNiagaraNodeCustomHlsl* N = Creator.CreateNode();
	N->NodePosX = PosX; N->NodePosY = PosY;
	Creator.Finalize();
	SetCustomHlslReflected(N, Code);

	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();

	R.bSuccess = true;
	R.Message  = TEXT("Custom HLSL node added");
	R.NodeId   = N->NodeGuid.ToString();
	return R;
}

bool UNiagaraScratchPadService::SetCustomHlslCode(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId,
	const FString& Code)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return false;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return false;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
	auto* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node);
	if (!Hlsl) return false;
	SetCustomHlslReflected(Hlsl, Code);
	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();
	return true;
}

FString UNiagaraScratchPadService::GetCustomHlslCode(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FString();
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return FString();
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
	if (auto* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node))
	{
		return GetCustomHlslReflected(Hlsl);
	}
	return FString();
}

FNiagaraScratchResult UNiagaraScratchPadService::AddPin(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId,
	const FString& Direction,
	const FString& TypeName,
	const FString& PinName)
{
	FNiagaraScratchResult R;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { R.Message = TEXT("System not found"); return R; }
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { R.Message = TEXT("Emitter not found"); return R; }
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
	if (!Node) { R.Message = TEXT("Node not found"); return R; }

	FNiagaraTypeDefinition Type;
	if (!ResolveType(TypeName, Type)) { R.Message = FString::Printf(TEXT("Unknown TypeName '%s'"), *TypeName); return R; }

	const EEdGraphPinDirection Dir = Direction.Equals(TEXT("Output"), ESearchCase::IgnoreCase) ? EGPD_Output : EGPD_Input;

	Node->Modify();

	// We bypass UNiagaraNodeWithDynamicPins::AddParameterPin / RequestNewTypedPin (declared in a
	// public header but not exported across modules) and call UEdGraphNode::CreatePin directly
	// with the pin type produced by UEdGraphSchema_Niagara::TypeDefinitionToPinType (exported).
	// For parameter-map nodes the pin name IS the variable name (already namespaced); for the
	// Custom HLSL node we trigger a signature rebuild via PostEditChangeProperty on CustomHlsl.
	const FEdGraphPinType PinType = MakeNiagaraPinType(Type);

	UEdGraphPin* NewPin = nullptr;
	if (IsMapGet(Node))
	{
		const FString Namespaced = NormalizeNamespacedName(PinName, TEXT("Module"));
		NewPin = Node->CreatePin(EGPD_Output, PinType, FName(*Namespaced));
		PlacePinBeforeAddPin(Node, NewPin);
	}
	else if (IsMapSet(Node))
	{
		const FString Namespaced = NormalizeNamespacedName(PinName, TEXT("Output"));
		NewPin = Node->CreatePin(EGPD_Input, PinType, FName(*Namespaced));
		PlacePinBeforeAddPin(Node, NewPin);
	}
	else if (Node->IsA<UNiagaraNodeCustomHlsl>())
	{
		NewPin = Node->CreatePin(Dir, PinType, FName(*PinName));
		PlacePinBeforeAddPin(Node, NewPin);
		// Rebuild the script signature so the new pin participates in compilation.
		// Must NOT route through PostEditChangeProperty - that calls RefreshFromExternalChanges
		// which calls ReallocatePins(false) which rebuilds pins from the stale signature
		// and wipes the new pin. We rebuild Signature directly from the current pin set.
		RebuildCustomHlslSignatureFromPins(Node);
	}
	else if (Cast<UNiagaraNodeWithDynamicPins>(Node) != nullptr)
	{
		// Generic dynamic-pin node (Op, If, etc.)
		NewPin = Node->CreatePin(Dir, PinType, FName(*PinName));
		PlacePinBeforeAddPin(Node, NewPin);
	}
	else
	{
		R.Message = TEXT("Node type does not support dynamic pins");
		return R;
	}

	if (!NewPin) { R.Message = TEXT("CreatePin returned null"); return R; }
	R.bSuccess = true; R.Message = NewPin->PinName.ToString(); R.NodeId = NodeId;

	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();
	return R;
}

bool UNiagaraScratchPadService::DeleteNode(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return false;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return false;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
	if (!Node) return false;
	Graph->Modify();
	Node->Modify();
	Node->BreakAllNodeLinks();
	Graph->RemoveNode(Node);
	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();
	return true;
}

bool UNiagaraScratchPadService::SetNodePosition(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId,
	int32 PosX,
	int32 PosY)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return false;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return false;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
	if (!Node) return false;
	Node->Modify();
	Node->NodePosX = PosX;
	Node->NodePosY = PosY;
	System->MarkPackageDirty();
	return true;
}

// =====================================================================
// Wiring
// =====================================================================

bool UNiagaraScratchPadService::ConnectPins(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& FromNodeId,
	const FString& FromPin,
	const FString& ToNodeId,
	const FString& ToPin)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return false;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return false;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) return false;

	UEdGraphNode* FromN = FindNodeByGuid(Graph, FromNodeId);
	UEdGraphNode* ToN   = FindNodeByGuid(Graph, ToNodeId);
	if (!FromN || !ToN) return false;

	UEdGraphPin* PinA = FindPinByName(FromN, FromPin, EGPD_Output, /*constrain*/true);
	UEdGraphPin* PinB = FindPinByName(ToN,   ToPin,   EGPD_Input,  /*constrain*/true);
	if (!PinA || !PinB) return false;

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema) return false;

	Graph->Modify();
	const bool bConnected = Schema->TryCreateConnection(PinA, PinB);
	if (bConnected)
	{
		Graph->NotifyGraphChanged();
		System->MarkPackageDirty();
	}
	return bConnected;
}

bool UNiagaraScratchPadService::DisconnectPin(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& NodeId,
	const FString& PinName)
{
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return false;
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) return false;
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
	if (!Node) return false;
	UEdGraphPin* Pin = FindPinByName(Node, PinName, EGPD_Input, /*constrain*/false);
	if (!Pin) return false;

	Graph->Modify();
	Pin->BreakAllPinLinks();
	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();
	return true;
}

// =====================================================================
// Module signature helpers
// =====================================================================

namespace
{
	/** Find any existing node of the given class in a graph (linear scan; graphs are small). */
	UEdGraphNode* FindFirstNodeOfClass(UNiagaraGraph* Graph, UClass* Cls)
	{
		if (!Graph || !Cls) return nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->IsA(Cls)) return N;
		}
		return nullptr;
	}

	/** Return an existing Map Get node from the graph, or spawn one. Returned as the public base class. */
	UNiagaraNodeWithDynamicPins* GetOrCreateMapGet(UNiagaraGraph* Graph)
	{
		UClass* Cls = GetMapGetClass();
		if (!Graph || !Cls) return nullptr;
		if (UEdGraphNode* Existing = FindFirstNodeOfClass(Graph, Cls))
		{
			return Cast<UNiagaraNodeWithDynamicPins>(Existing);
		}
		Graph->Modify();
		return Cast<UNiagaraNodeWithDynamicPins>(SpawnNodeByClass(Graph, Cls, -200, 0));
	}

	UNiagaraNodeWithDynamicPins* GetOrCreateMapSet(UNiagaraGraph* Graph)
	{
		UClass* Cls = GetMapSetClass();
		if (!Graph || !Cls) return nullptr;
		if (UEdGraphNode* Existing = FindFirstNodeOfClass(Graph, Cls))
		{
			return Cast<UNiagaraNodeWithDynamicPins>(Existing);
		}
		Graph->Modify();
		return Cast<UNiagaraNodeWithDynamicPins>(SpawnNodeByClass(Graph, Cls, 400, 0));
	}
}

FNiagaraScratchResult UNiagaraScratchPadService::AddModuleInput(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& InputName,
	const FString& TypeName)
{
	FNiagaraScratchResult R;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { R.Message = TEXT("System not found"); return R; }
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { R.Message = TEXT("Emitter not found"); return R; }
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) { R.Message = TEXT("Scratch module not found"); return R; }

	FNiagaraTypeDefinition Type;
	if (!ResolveType(TypeName, Type)) { R.Message = FString::Printf(TEXT("Unknown TypeName '%s'"), *TypeName); return R; }

	UNiagaraNodeWithDynamicPins* MapGet = GetOrCreateMapGet(Graph);
	if (!MapGet) { R.Message = TEXT("Could not find/create Map Get"); return R; }

	const FString Namespaced = NormalizeNamespacedName(InputName, TEXT("Module"));

	// Idempotent: a Module.<name> output pin may already exist (a repeated call, or a re-run of
	// the same setup). Reuse it instead of creating a second pin with the same name - duplicate
	// Map Get outputs corrupt the module signature and break compilation.
	for (UEdGraphPin* P : MapGet->Pins)
	{
		if (P && P->Direction == EGPD_Output && P->PinName == FName(*Namespaced))
		{
			R.bSuccess = true;
			R.Message  = P->PinName.ToString();
			R.NodeId   = MapGet->NodeGuid.ToString();
			return R;
		}
	}

	MapGet->Modify();
	const FEdGraphPinType PinType = MakeNiagaraPinType(Type);
	UEdGraphPin* NewPin = MapGet->CreatePin(EGPD_Output, PinType, FName(*Namespaced));
	if (!NewPin) { R.Message = TEXT("CreatePin returned null"); return R; }
	PlacePinBeforeAddPin(MapGet, NewPin);

	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();

	R.bSuccess = true;
	R.Message  = NewPin->PinName.ToString();
	R.NodeId   = MapGet->NodeGuid.ToString();
	return R;
}

FNiagaraScratchResult UNiagaraScratchPadService::AddModuleOutput(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& ModuleName,
	const FString& OutputName,
	const FString& TypeName)
{
	FNiagaraScratchResult R;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) { R.Message = TEXT("System not found"); return R; }
	FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, EmitterName);
	if (!Handle) { R.Message = TEXT("Emitter not found"); return R; }
	UNiagaraNodeFunctionCall* Call = FindModuleNodeOnEmitter(Handle->GetEmitterData(), ModuleName);
	UNiagaraGraph* Graph = GetScratchGraphFromCall(Call);
	if (!Graph) { R.Message = TEXT("Scratch module not found"); return R; }

	FNiagaraTypeDefinition Type;
	if (!ResolveType(TypeName, Type)) { R.Message = FString::Printf(TEXT("Unknown TypeName '%s'"), *TypeName); return R; }

	UNiagaraNodeWithDynamicPins* MapSet = GetOrCreateMapSet(Graph);
	if (!MapSet) { R.Message = TEXT("Could not find/create Map Set"); return R; }

	const FString Namespaced = NormalizeNamespacedName(OutputName, TEXT("Output"));

	// Idempotent: a Particles.<name> (or Output.<name>) write pin may already exist. Reuse it
	// instead of adding a duplicate. Two input pins of the same name on the Map Set both wired
	// to a source produce duplicate writes to one attribute, which makes the compiled system
	// invalid ("System is invalid after compilation"). This is the single most common way a
	// caller corrupts the stack: calling add_module_output more than once for the same output.
	for (UEdGraphPin* P : MapSet->Pins)
	{
		if (P && P->Direction == EGPD_Input && P->PinName == FName(*Namespaced))
		{
			R.bSuccess = true;
			R.Message  = P->PinName.ToString();
			R.NodeId   = MapSet->NodeGuid.ToString();
			return R;
		}
	}

	MapSet->Modify();
	const FEdGraphPinType PinType = MakeNiagaraPinType(Type);
	UEdGraphPin* NewPin = MapSet->CreatePin(EGPD_Input, PinType, FName(*Namespaced));
	if (!NewPin) { R.Message = TEXT("CreatePin returned null"); return R; }
	PlacePinBeforeAddPin(MapSet, NewPin);

	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();

	R.bSuccess = true;
	R.Message  = NewPin->PinName.ToString();
	R.NodeId   = MapSet->NodeGuid.ToString();
	return R;
}

// =====================================================================
// Apply
// =====================================================================

namespace
{
	/**
	 * The stack view (opening the system, GetScriptStackTopology, GetStackIssues) walks every scratch
	 * graph through BuildParameterMapHistory, which asserts on shapes the compiler accepts. Check one
	 * dynamic-pin node for those shapes without building a stack view. With bRepair, fix what is safe
	 * to fix - add pins out of place (pin order only) and a Custom HLSL signature out of step with its
	 * pins (rebuilt from the pins, as the engine's RebuildSignatureFromPins does) - and list each
	 * repair in OutRepairs. Whatever is still wrong goes to OutErrors.
	 */
	void CheckNodeStackShape(UEdGraphNode* Node, const FString& Where, bool bRepair, TArray<FString>& OutErrors, TArray<FString>& OutRepairs)
	{
		if (!Node || !Cast<UNiagaraNodeWithDynamicPins>(Node)) return;

		if (!HasAddPinsLast(Node))
		{
			if (bRepair && MoveAddPinsLast(Node))
			{
				OutRepairs.Add(FString::Printf(TEXT("%s: moved the add pins behind the real pins."), *Where));
			}
			else
			{
				OutErrors.Add(FString::Printf(TEXT("%s: a real pin sits behind the add pin; building the stack view would crash the editor. Run verify_stack(path, True) to repair the pin order."), *Where));
				return;
			}
		}

		auto* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Node);
		if (!Hlsl) return;

		TArray<FName> InputNames;
		TArray<FName> OutputNames;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || IsDynamicAddPin(Pin)) continue;
			(Pin->Direction == EGPD_Input ? InputNames : OutputNames).Add(Pin->PinName);
		}
		// Signature inputs are FNiagaraVariable, outputs FNiagaraVariableBase.
		auto SignatureMatches = [](const TArray<FName>& Names, const auto& Vars)
		{
			if (Names.Num() != Vars.Num()) return false;
			for (int32 i = 0; i < Names.Num(); ++i)
			{
				if (Names[i] != Vars[i].GetName()) return false;
			}
			return true;
		};
		const FNiagaraFunctionSignature& Sig = Hlsl->Signature;
		if (!SignatureMatches(InputNames, Sig.Inputs) || !SignatureMatches(OutputNames, Sig.Outputs))
		{
			const FString Counts = FString::Printf(TEXT("pins %d in / %d out, signature %d in / %d out"),
				InputNames.Num(), OutputNames.Num(), Sig.Inputs.Num(), Sig.Outputs.Num());
			if (bRepair)
			{
				RebuildCustomHlslSignatureFromPins(Hlsl);
				OutRepairs.Add(FString::Printf(TEXT("%s: rebuilt the Custom HLSL signature from its pins (%s)."), *Where, *Counts));
			}
			else
			{
				OutErrors.Add(FString::Printf(TEXT("%s: Custom HLSL pins and signature differ (%s); the stack view pairs them by index. Run verify_stack(path, True) to rebuild the signature."), *Where, *Counts));
			}
		}
	}

	/** CheckNodeStackShape over every node of every emitter scratch module. */
	void CheckStackShapes(UNiagaraSystem* System, bool bRepair, TArray<FString>& OutErrors, TArray<FString>& OutRepairs)
	{
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (!EmitterData || !EmitterData->ScratchPads) continue;
			for (UNiagaraScript* Scratch : EmitterData->ScratchPads->Scripts)
			{
				if (!Scratch) continue;
				const auto* Src = Cast<UNiagaraScriptSource>(Scratch->GetLatestSource());
				if (!Src || !Src->NodeGraph) continue;

				for (UEdGraphNode* Node : Src->NodeGraph->Nodes)
				{
					if (!Node || !Cast<UNiagaraNodeWithDynamicPins>(Node)) continue;
					const FString Where = FString::Printf(TEXT("Scratch module '%s' (emitter '%s'), %s node %s"),
						*Scratch->GetName(), *Handle.GetName().ToString(), *GetNodeTypeLabel(Node), *Node->NodeGuid.ToString());
					CheckNodeStackShape(Node, Where, bRepair, OutErrors, OutRepairs);
				}
			}
		}
	}

	// A scratch module left with duplicate same-named pins on a Map Get/Set node (the
	// pre-7bca8ce non-idempotency, or manual edits) makes ANY later system compile fire
	// a fatal "Array index out of bounds" assert inside ReallocatePins — taking down the
	// editor. Detect it up front so ApplyChanges can refuse with a clear error instead
	// (issue #432).
	bool ValidateScratchGraphsForCompile(UNiagaraSystem* System, FString& OutError)
	{
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (!EmitterData || !EmitterData->ScratchPads) continue;
			for (UNiagaraScript* Scratch : EmitterData->ScratchPads->Scripts)
			{
				if (!Scratch) continue;
				const auto* Src = Cast<UNiagaraScriptSource>(Scratch->GetLatestSource());
				if (!Src || !Src->NodeGraph) continue;

				for (UEdGraphNode* Node : Src->NodeGraph->Nodes)
				{
					if (!IsMapGet(Node) && !IsMapSet(Node)) continue;

					TSet<FName> InputNames;
					TSet<FName> OutputNames;
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin) continue;
						TSet<FName>& Names = (Pin->Direction == EGPD_Input) ? InputNames : OutputNames;
						bool bAlreadyThere = false;
						Names.Add(Pin->PinName, &bAlreadyThere);
						if (bAlreadyThere)
						{
							OutError = FString::Printf(
								TEXT("Scratch module '%s' (emitter '%s') has duplicate %s pins named '%s' on a %s node (%s) — compiling this graph would crash the editor. Remove the duplicate pin (remove_node_pin) before apply_changes."),
								*Scratch->GetName(), *Handle.GetName().ToString(),
								Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
								*Pin->PinName.ToString(),
								IsMapGet(Node) ? TEXT("MapGet") : TEXT("MapSet"),
								*Node->NodeGuid.ToString());
							return false;
						}
					}
				}
			}
		}
		return true;
	}

	// Collect the real Niagara compiler diagnostics from every script in the system.
	void CollectCompileMessages(UNiagaraSystem* System, bool bErrorsOnly, TArray<FString>& OutMessages)
	{
		TArray<TPair<FString, UNiagaraScript*>> Scripts;
		Scripts.Emplace(TEXT("SystemSpawn"), System->GetSystemSpawnScript());
		Scripts.Emplace(TEXT("SystemUpdate"), System->GetSystemUpdateScript());
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (!EmitterData) continue;
			TArray<UNiagaraScript*> EmitterScripts;
			EmitterData->GetScripts(EmitterScripts, /*bCompilableOnly=*/true);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				Scripts.Emplace(Handle.GetName().ToString(), Script);
			}
		}

		for (const TPair<FString, UNiagaraScript*>& Entry : Scripts)
		{
			UNiagaraScript* Script = Entry.Value;
			if (!Script) continue;

#if WITH_EDITORONLY_DATA
			const FNiagaraVMExecutableData& VMData = Script->GetVMExecutableData();
			if (VMData.LastCompileStatus == ENiagaraScriptCompileStatus::NCS_Error && !VMData.ErrorMsg.IsEmpty())
			{
				OutMessages.Add(FString::Printf(TEXT("Error: [%s/%s] %s"), *Entry.Key, *Script->GetName(), *VMData.ErrorMsg));
			}
			for (const FNiagaraCompileEvent& Event : VMData.LastCompileEvents)
			{
				const bool bIsError = Event.Severity == FNiagaraCompileEventSeverity::Error;
				const bool bIsWarning = Event.Severity == FNiagaraCompileEventSeverity::Warning;
				if (!bIsError && (bErrorsOnly || !bIsWarning))
				{
					continue;
				}
				OutMessages.Add(FString::Printf(TEXT("%s: [%s/%s] %s%s"),
					bIsError ? TEXT("Error") : TEXT("Warning"),
					*Entry.Key, *Script->GetName(),
					*Event.Message,
					Event.NodeGuid.IsValid() ? *FString::Printf(TEXT(" (node %s)"), *Event.NodeGuid.ToString()) : TEXT("")));
			}
#endif
		}
	}

	/**
	 * Build the system's stack view the way the Niagara editor does and collect its issues. This is
	 * the same path that crashed on a misordered Custom HLSL node, so call it only after
	 * CheckStackShapes found nothing: it then proves the stack view really builds, and reports what
	 * the editor's stack would show (missing inputs, bad links, script errors). Warnings and info
	 * are added only when OutWarnings is given.
	 */
	void CollectStackViewIssues(UNiagaraSystem* System, TArray<FString>& OutErrors, TArray<FString>* OutWarnings)
	{
		if (System->HasActiveCompilations() || System->HasOutstandingCompilationRequests(false))
		{
			System->WaitForCompilationComplete();
		}
		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_StackIssues Issues;
		UNiagaraExternalEditUtilities::GetStackIssues(System, Issues, Context);
		for (const FText& Error : Context.Errors)
		{
			OutErrors.Add(FString::Printf(TEXT("Stack view: %s"), *Error.ToString()));
		}
		for (const FNiagaraExt_StackIssue& Issue : Issues.Issues)
		{
			const FString Line = FString::Printf(TEXT("[%s] %s%s%s"), *Issue.StackDisplayPath, *Issue.ShortDescription,
				Issue.LongDescription.IsEmpty() ? TEXT("") : TEXT(" - "), *Issue.LongDescription);
			if (Issue.Severity == ENiagaraExt_StackIssueSeverity::Error)
			{
				OutErrors.Add(TEXT("Stack error: ") + Line);
			}
			else if (OutWarnings && Issue.Severity != ENiagaraExt_StackIssueSeverity::None)
			{
				OutWarnings->Add((Issue.Severity == ENiagaraExt_StackIssueSeverity::Warning ? TEXT("Stack warning: ") : TEXT("Stack info: ")) + Line);
			}
		}
	}

	// Messages from the last ApplyChanges, per system path; read through GetLastApplyReport.
	TMap<FString, TArray<FString>> GScratchPadLastApplyReport;
}

bool UNiagaraScratchPadService::ApplyChanges(const FString& SystemPath)
{
	TArray<FString>& Report = GScratchPadLastApplyReport.FindOrAdd(SystemPath);
	Report.Reset();
	auto Fail = [&Report](const FString& Msg)
	{
		UE_LOG(LogTemp, Error, TEXT("ApplyChanges: %s"), *Msg);
		Report.Add(TEXT("Error: ") + Msg);
	};

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System)
	{
		Fail(FString::Printf(TEXT("failed to load Niagara system '%s'"), *SystemPath));
		return false;
	}

	// Refuse to compile a corrupt scratch graph — the engine assert it would fire is fatal.
	FString ValidationError;
	if (!ValidateScratchGraphsForCompile(System, ValidationError))
	{
		Fail(ValidationError);
		return false;
	}

	// Repair the shapes the stack view would assert on (safe repairs only), refuse the rest. The
	// compile alone does not catch them: it passed on the graph that later crashed the editor.
	TArray<FString> ShapeErrors;
	TArray<FString> Repairs;
	CheckStackShapes(System, /*bRepair=*/true, ShapeErrors, Repairs);
	for (const FString& Repair : Repairs)
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyChanges: repaired %s"), *Repair);
		Report.Add(TEXT("Repaired: ") + Repair);
	}
	if (ShapeErrors.Num() > 0)
	{
		for (const FString& Msg : ShapeErrors) Fail(Msg);
		return false;
	}

	// Notify each scratch script graph that it has changed so its ChangeID bumps. This is what
	// triggers callers (the stack function-call nodes that reference it) to pick up the new
	// signature on next compile/refresh.
	//
	// We intentionally do NOT call FunctionCall->RefreshFromExternalChanges() on the stack-level
	// function-call nodes here. That path runs ReallocatePins(false) which in some signature
	// transitions (e.g. a scratch module growing new Module.* inputs) asserts inside the engine
	// with "Array index out of bounds: 1 into an array of size 1". The full RequestCompile +
	// WaitForCompilationComplete below performs the same rebuild safely - the system compile
	// re-reads each function call's referenced script graph.
	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData || !EmitterData->ScratchPads) continue;
		for (UNiagaraScript* Scratch : EmitterData->ScratchPads->Scripts)
		{
			if (!Scratch) continue;
			if (auto* Src = Cast<UNiagaraScriptSource>(Scratch->GetLatestSource()))
			{
				if (Src->NodeGraph)
				{
					Src->NodeGraph->NotifyGraphChanged();
				}
			}
			Scratch->MarkPackageDirty();
		}
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);
	System->WaitForCompilationComplete();

	// Surface the real compiler diagnostics instead of a bare true/false — a compile
	// with errors is a failure the caller must see (issues #432, #352).
	TArray<FString> CompileErrors;
	CollectCompileMessages(System, /*bErrorsOnly=*/true, CompileErrors);
	for (const FString& Msg : CompileErrors)
	{
		UE_LOG(LogTemp, Error, TEXT("ApplyChanges: %s"), *Msg);
		Report.Add(Msg);
	}

	// A clean compile does not prove the stack view builds; build it and read its errors.
	TArray<FString> StackErrors;
	CollectStackViewIssues(System, StackErrors, nullptr);
	for (const FString& Msg : StackErrors) Fail(Msg);

	// Save only a system that compiled and whose stack view built cleanly: a saved broken module
	// crashes whoever opens the system next. A failed system stays dirty in memory.
	if (CompileErrors.Num() > 0 || StackErrors.Num() > 0)
	{
		Report.Add(TEXT("Not saved: the system stays dirty in memory until the errors are fixed."));
		return false;
	}
	UEditorAssetLibrary::SaveLoadedAsset(System);
	return true;
}

TArray<FString> UNiagaraScratchPadService::GetLastApplyReport(const FString& SystemPath)
{
	const TArray<FString>* Report = GScratchPadLastApplyReport.Find(SystemPath);
	return Report ? *Report : TArray<FString>();
}

TArray<FString> UNiagaraScratchPadService::VerifyStack(const FString& SystemPath, bool bRepair, bool bIncludeWarnings)
{
	TArray<FString> Lines;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System)
	{
		Lines.Add(FString::Printf(TEXT("Error: failed to load Niagara system '%s'"), *SystemPath));
		return Lines;
	}

	TArray<FString> Errors;
	TArray<FString> Repairs;
	FString DuplicateError;
	if (!ValidateScratchGraphsForCompile(System, DuplicateError))
	{
		Errors.Add(DuplicateError);
	}
	CheckStackShapes(System, bRepair, Errors, Repairs);
	for (const FString& Repair : Repairs)
	{
		Lines.Add(TEXT("Repaired: ") + Repair);
	}
	if (Repairs.Num() > 0)
	{
		System->MarkPackageDirty();
	}

	// Building the stack view on a graph with a known crash shape would take the editor down;
	// report the shapes and stop there.
	if (Errors.Num() > 0)
	{
		for (const FString& Msg : Errors) Lines.Add(TEXT("Error: ") + Msg);
		Lines.Add(TEXT("Error: stack view not built (fix the errors above first)."));
		return Lines;
	}

	TArray<FString> StackErrors;
	TArray<FString> StackWarnings;
	CollectStackViewIssues(System, StackErrors, bIncludeWarnings ? &StackWarnings : nullptr);
	for (const FString& Msg : StackErrors) Lines.Add(TEXT("Error: ") + Msg);
	for (const FString& Msg : StackWarnings) Lines.Add(TEXT("Warning: ") + Msg);
	return Lines;
}

TArray<FString> UNiagaraScratchPadService::GetCompileMessages(const FString& SystemPath, bool bErrorsOnly)
{
	TArray<FString> Messages;
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System)
	{
		Messages.Add(FString::Printf(TEXT("Error: failed to load Niagara system '%s'"), *SystemPath));
		return Messages;
	}
	CollectCompileMessages(System, bErrorsOnly, Messages);
	return Messages;
}

#if WITH_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

// A pin made with a bare CreatePin lands behind the "+" add pin, and the stack view then reads past
// the end of the Custom HLSL signature (editor crash). The check must see that shape, the repair must
// fix it, and the service's own pin placement must never make it. Runs on a transient graph: no
// asset, no compile, no stack view.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVibeNiagaraScratchAddPinOrderTest, "VibeUE.Niagara.ScratchPad.AddPinOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVibeNiagaraScratchAddPinOrderTest::RunTest(const FString& Parameters)
{
	UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(GetTransientPackage(), NAME_None, RF_Transient);
	FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(*Graph);
	UNiagaraNodeCustomHlsl* Node = Creator.CreateNode();
	Creator.Finalize();
	if (!TestNotNull(TEXT("Custom HLSL node"), Node)) return false;

	int32 AddPins = 0;
	for (const UEdGraphPin* Pin : Node->Pins) { AddPins += IsDynamicAddPin(Pin) ? 1 : 0; }
	if (!TestTrue(TEXT("a new Custom HLSL node has add pins (positive control for the order checks)"), AddPins > 0)) return false;
	TestTrue(TEXT("a new node already has its add pins last"), HasAddPinsLast(Node));

	const FEdGraphPinType FloatPin = MakeNiagaraPinType(FNiagaraTypeDefinition::GetFloatDef());
	TArray<FString> Errors;
	TArray<FString> Repairs;

	// The old service: a bare CreatePin, then the signature rebuilt from the pins.
	Node->CreatePin(EGPD_Input, FloatPin, TEXT("A"));
	RebuildCustomHlslSignatureFromPins(Node);
	TestFalse(TEXT("a bare CreatePin leaves the pin behind the add pin"), HasAddPinsLast(Node));
	CheckNodeStackShape(Node, TEXT("test"), /*bRepair=*/false, Errors, Repairs);
	TestEqual(TEXT("the check reports the misplaced pin"), Errors.Num(), 1);
	TestEqual(TEXT("a check without repair repairs nothing"), Repairs.Num(), 0);
	TestFalse(TEXT("a check without repair leaves the order as it was"), HasAddPinsLast(Node));

	Errors.Reset();
	CheckNodeStackShape(Node, TEXT("test"), /*bRepair=*/true, Errors, Repairs);
	TestEqual(TEXT("repair leaves no error"), Errors.Num(), 0);
	TestEqual(TEXT("repair reports one repair"), Repairs.Num(), 1);
	TestTrue(TEXT("repair puts the add pins last"), HasAddPinsLast(Node));

	// The fixed service: CreatePin, then PlacePinBeforeAddPin.
	UEdGraphPin* B = Node->CreatePin(EGPD_Input, FloatPin, TEXT("B"));
	PlacePinBeforeAddPin(Node, B);
	TestTrue(TEXT("PlacePinBeforeAddPin keeps the add pins last"), HasAddPinsLast(Node));

	// A pin the signature does not know yet: the stack view pairs pins and signature by index.
	Errors.Reset();
	Repairs.Reset();
	CheckNodeStackShape(Node, TEXT("test"), /*bRepair=*/false, Errors, Repairs);
	TestEqual(TEXT("the check reports a signature out of step with the pins"), Errors.Num(), 1);
	CheckNodeStackShape(Node, TEXT("test"), /*bRepair=*/true, Errors, Repairs);
	TestEqual(TEXT("repair rebuilds the signature"), Repairs.Num(), 1);
	if (TestEqual(TEXT("signature inputs after repair"), Node->Signature.Inputs.Num(), 2))
	{
		TestEqual(TEXT("signature input 0"), Node->Signature.Inputs[0].GetName(), FName(TEXT("A")));
		TestEqual(TEXT("signature input 1"), Node->Signature.Inputs[1].GetName(), FName(TEXT("B")));
	}

	Errors.Reset();
	Repairs.Reset();
	CheckNodeStackShape(Node, TEXT("test"), /*bRepair=*/false, Errors, Repairs);
	TestEqual(TEXT("a repaired node checks clean"), Errors.Num(), 0);
	return true;
}
#endif // WITH_AUTOMATION_TESTS

#undef LOCTEXT_NAMESPACE
