// Copyright 2016 Vladimir Alyamkin. All Rights Reserved.

#include "VtaEditorPlugin.h"
#include "VtaTextureAtlasDataModel.h"

#include "PackageTools.h"
#include "Runtime/Launch/Resources/Version.h"
#include "IAssetTools.h"

#define LOCTEXT_NAMESPACE "VtaEditorPlugin"

//////////////////////////////////////////////////////////////////////////
// UVtaTextureAtlasImportFactory

UVtaTextureAtlasImportFactory::UVtaTextureAtlasImportFactory(const class FObjectInitializer& PCIP)
	: Super(PCIP)
	, bIsReimporting(false)
	, ExistingAtlasTexture(nullptr)
{
	SupportedClass = UVtaTextureAtlas::StaticClass();
	bCreateNew = false;
	bEditAfterNew = true;

	bEditorImport = true;
	bText = true;

	Formats.Add(TEXT("vta;VaTexAtlas data file"));
	Formats.Add(TEXT("json;VaTexAtlas JSON file"));
	
	ExistingTable = nullptr;
}

FText UVtaTextureAtlasImportFactory::GetToolTip() const
{
	return LOCTEXT("VtaTextureAtlasImportFactoryDescription", "Texture atlas imported from TexturePacker");
}

bool UVtaTextureAtlasImportFactory::FactoryCanImport(const FString& Filename)
{
	FString FileContent;
	if (FFileHelper::LoadFileToString(FileContent, *Filename))
	{
		TSharedPtr<FJsonObject> DescriptorObject = ParseJSON(FileContent, FString(), true);
		if (DescriptorObject.IsValid())
		{
			FVtaDataFile GlobalInfo;
			GlobalInfo.ParseFromJSON(DescriptorObject, Filename, true, true);

			return GlobalInfo.IsValid();
		}
	}

	return false;
}

UObject* UVtaTextureAtlasImportFactory::FactoryCreateText(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, const TCHAR* Type, const TCHAR*& Buffer, const TCHAR* BufferEnd, FFeedbackContext* Warn)
{
	auto Settings = GetMutableDefault<UVtaEditorPluginSettings>();
	
	Flags |= RF_Transactional;

	FEditorDelegates::OnAssetPreImport.Broadcast(this, InClass, InParent, InName, Type);

	FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

#if ENGINE_MINOR_VERSION <= 12
	const FString CurrentFilename = UFactory::GetCurrentFilename();
#endif
	FString CurrentSourcePath;
	FString FilenameNoExtension;
	FString UnusedExtension;
	FPaths::Split(CurrentFilename, CurrentSourcePath, FilenameNoExtension, UnusedExtension);

	const FString LongPackagePath = FPackageName::GetLongPackagePath(InParent->GetOutermost()->GetPathName());

	const FString NameForErrors(InName.ToString());
	const FString FileContent(BufferEnd - Buffer, Buffer);
	TSharedPtr<FJsonObject> DescriptorObject = ParseJSON(FileContent, NameForErrors);

	// Clear existing atlas
	UVtaTextureAtlas* ExistingAtlas = FindObject<UVtaTextureAtlas>(InParent, *InName.ToString());
	if (ExistingAtlas)
	{
		ExistingAtlas->EmptyData();
	}

	UVtaTextureAtlas* Result = nullptr;
	UTexture2D* ImageTexture = nullptr;

	// Parse the file 
	FVtaDataFile DataModel;
	if (DescriptorObject.IsValid())
	{
		DataModel.ParseFromJSON(DescriptorObject, NameForErrors, false, false);
	}

	// Create the new 'hub' asset and convert the data model over
	if (DataModel.IsValid())
	{
		const FString TexturesSubPath = LongPackagePath / TEXT("Textures");
		const FString FramesSubPath = LongPackagePath / TEXT("Frames");

		// Create asset
		Result = NewObject<UVtaTextureAtlas>(InParent, InName, Flags);
		Result->Modify();

		// Save filename paths
		Result->AssetImportData->Update(CurrentFilename);

		// Cache data for debug
		Result->ImportedData = DataModel;

		// Load the base texture
		const FString SourceAtlasTextureFilename = FPaths::Combine(*CurrentSourcePath, *DataModel.Meta.Image);
		ImageTexture = ImportOrReimportTexture((bIsReimporting && (ExistingAtlasTextureName == DataModel.Meta.Image)) ? ExistingAtlasTexture : nullptr, SourceAtlasTextureFilename, TexturesSubPath);
		if (ImageTexture == nullptr)
		{
			UE_LOG(LogVaTexAtlasEditor, Warning, TEXT("Failed to import atlas image '%s'."), *SourceAtlasTextureFilename);
		}

		// Load parent material for frames
		UMaterial* FrameMaterial = LoadObject<UMaterial>(NULL, TEXT("/VaTexAtlasPlugin/Materials/M_AtlasFrame.M_AtlasFrame"), NULL, LOAD_None, NULL);

		GWarn->BeginSlowTask(LOCTEXT("VtaTextureAtlasImportFactory_ImportingFrames", "Importing Atlas Frames"), true, true);

		// Perform assets import
		for (int32 i = 0; i < DataModel.Frames.Num(); i++)
		{
			auto Frame = DataModel.Frames[i];
			const FString FrameAssetName = BuildFrameName(NameForErrors, Frame.Filename);
			const FString SlateTextureAssetName = BuildSlateTextureName(NameForErrors, Frame.Filename);

			GWarn->StatusUpdate(i, DataModel.Frames.Num(), LOCTEXT("VtaTextureAtlasImportFactory_ImportingFrames", "Importing Atlas Frame"));

			// Check for user canceling the import
			if (GWarn->ReceivedUserCancel())
			{
				break;
			}

			// Calculate UVs in linear space as (U, UMax, V, VMax)
			FLinearColor FrameUVs = FLinearColor::Black;
			FrameUVs.R = (float)Frame.Frame.X / DataModel.Meta.Size.W;
			FrameUVs.G = FrameUVs.R + (float)Frame.Frame.W / DataModel.Meta.Size.W;
			FrameUVs.B = (float)Frame.Frame.Y / DataModel.Meta.Size.H;
			FrameUVs.A = FrameUVs.B + (float)Frame.Frame.H / DataModel.Meta.Size.H;

			// Create a frame in the package
			UMaterialInstanceConstant* TargetFrame = nullptr;
			UVtaSlateTexture* TargetSlateTexture = nullptr;

			// Check we have existing frame asset
			if (bIsReimporting)
			{
				TargetFrame = FindExistingFrame(Frame.Filename);
				if (TargetFrame)
				{
					TargetFrame->Modify();
				}
				else
				{
					UE_LOG(LogVaTexAtlasEditor, Error, TEXT("Failed to load existing frame: '%s'"), *Frame.Filename);
				}
				
				TargetSlateTexture = FindExistingSlateTexture(Frame.Filename);
				if (TargetSlateTexture)
				{
					TargetSlateTexture->Modify();
				}
				else
				{
					UE_LOG(LogVaTexAtlasEditor, Error, TEXT("Failed to load existing slate texture: '%s'"), *Frame.Filename);
				}
			}
			
			// Check we should create new one
			if (TargetFrame == nullptr && Settings->bGenerateMaterialInstances)
			{
				TargetFrame = CastChecked<UMaterialInstanceConstant>(CreateNewAsset(UMaterialInstanceConstant::StaticClass(), FramesSubPath, FrameAssetName, Flags));
			}
			
			if (TargetSlateTexture == nullptr && Settings->bGenerateSlateTextures)
			{
				TargetSlateTexture = CastChecked<UVtaSlateTexture>(CreateNewAsset(UVtaSlateTexture::StaticClass(), FramesSubPath, SlateTextureAssetName, Flags));
			}

			if (TargetFrame)
			{
				// Fill parameters for frame
				TargetFrame->SetParentEditorOnly(FrameMaterial);
				TargetFrame->SetTextureParameterValueEditorOnly(TEXT("Atlas"), ImageTexture);
				TargetFrame->SetVectorParameterValueEditorOnly(TEXT("FrameUV"), FrameUVs);
				
				// Make sure that changes are applied to assets
				FPropertyChangedEvent FinalRebuildFrameSet(nullptr, EPropertyChangeType::ValueSet);
				TargetFrame->PostEditChangeProperty(FinalRebuildFrameSet);
				
				// Set frame to Atlas
				Result->Frames.Add(TargetFrame);
			}
			
			if (TargetSlateTexture)
			{
				// Fill parameters for slate texture
				TargetSlateTexture->AtlasTexture = ImageTexture;
				TargetSlateTexture->StartUV = FVector2D(FrameUVs.R, FrameUVs.B);
				TargetSlateTexture->SizeUV = FVector2D(FrameUVs.G - FrameUVs.R, FrameUVs.A - FrameUVs.B);
				
				// Make sure that changes are applied to assets
				FPropertyChangedEvent FinalRebuildSlateTextureSet(nullptr, EPropertyChangeType::ValueSet);
				TargetSlateTexture->PostEditChangeProperty(FinalRebuildSlateTextureSet);
				
				// Set slate texture to Atlas
				Result->SlateTextures.Add(TargetSlateTexture);
			}
			// Set frame to Atlas
			Result->FrameNames.Add(Frame.Filename);
		}

		// Set data to atlas asset
		Result->TextureName = DataModel.Meta.Image;
		Result->Texture = ImageTexture;
		Result->PostEditChange();

		GWarn->EndSlowTask();
	}
	
	ImportOrReimportDataTable(Cast<UVtaTextureAtlas>(Result), LongPackagePath, InName.ToString(), Flags);
	
	FEditorDelegates::OnAssetPostImport.Broadcast(this, Result);

	// Reset the importer to ensure that no leftover data can contaminate future imports
	ResetImportData();

	return Result;
}

TSharedPtr<FJsonObject> UVtaTextureAtlasImportFactory::ParseJSON(const FString& FileContents, const FString& NameForErrors, bool bSilent)
{
	// Load the file up (JSON format)
	if (!FileContents.IsEmpty())
	{
		const TSharedRef< TJsonReader<> >& Reader = TJsonReaderFactory<>::Create(FileContents);

		TSharedPtr<FJsonObject> DescriptorObject;
		if (FJsonSerializer::Deserialize(Reader, DescriptorObject) && DescriptorObject.IsValid())
		{
			// File was loaded and deserialized OK!
			return DescriptorObject;
		}
		else
		{
			if (!bSilent)
			{
				UE_LOG(LogVaTexAtlasEditor, Error, TEXT("Failed to parse Texture Atlas '%s'. Error: '%s'"), *NameForErrors, *Reader->GetErrorMessage());
			}

			return nullptr;
		}
	}
	else
	{
		if (!bSilent)
		{
			UE_LOG(LogVaTexAtlasEditor, Error, TEXT("VTA data file '%s' was empty. This texture atlas cannot be imported."), *NameForErrors);
		}

		return nullptr;
	}
}

UObject* UVtaTextureAtlasImportFactory::CreateNewAsset(UClass* AssetClass, const FString& TargetPath, const FString& DesiredName, EObjectFlags Flags)
{
	FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

	// Create a unique package name and asset name for the frame
	const FString TentativePackagePath = PackageTools::SanitizePackageName(TargetPath + TEXT("/") + DesiredName);
	FString DefaultSuffix;
	FString AssetName;
	FString PackageName;
	AssetToolsModule.Get().CreateUniqueAssetName(TentativePackagePath, DefaultSuffix, PackageName, AssetName);

	// Create a package for the asset
	UObject* OuterForAsset = CreatePackage(nullptr, *PackageName);

	// Create a frame in the package
	UObject* NewAsset = NewObject<UObject>(OuterForAsset, AssetClass, *AssetName, Flags);
	FAssetRegistryModule::AssetCreated(NewAsset);

	NewAsset->Modify();
	return NewAsset;
}

UObject* UVtaTextureAtlasImportFactory::ImportAsset(const FString& SourceFilename, const FString& TargetSubPath)
{
	FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

	TArray<FString> FileNames;
	FileNames.Add(SourceFilename);

	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssets(FileNames, TargetSubPath);
	return (ImportedAssets.Num() > 0) ? ImportedAssets[0] : nullptr;
}

UTexture2D* UVtaTextureAtlasImportFactory::ImportTexture(const FString& SourceFilename, const FString& TargetSubPath)
{
	UTexture2D* ImportedTexture = Cast<UTexture2D>(ImportAsset(SourceFilename, TargetSubPath));

	if (ImportedTexture != nullptr)
	{
		ImportedTexture->Modify();

		// Default valus are used for UI icons
		ImportedTexture->LODGroup = TEXTUREGROUP_UI;
		ImportedTexture->CompressionSettings = TC_EditorIcon;

		ImportedTexture->PostEditChange();
	}

	return ImportedTexture;
}

UTexture2D* UVtaTextureAtlasImportFactory::ImportOrReimportTexture(UTexture2D* ExistingTexture, const FString& SourceFilename, const FString& TargetSubPath)
{
	UTexture2D* ResultTexture = nullptr;

	// Try reimporting if we have an existing texture
	if (ExistingTexture != nullptr)
	{
		if (FReimportManager::Instance()->Reimport(ExistingTexture, true))
		{
			ResultTexture = ExistingTexture;
		}
	}

	// If that fails, import the original textures
	if (ResultTexture == nullptr)
	{
		ResultTexture = ImportTexture(SourceFilename, TargetSubPath);
	}

	return ResultTexture;
}

void UVtaTextureAtlasImportFactory::ImportOrReimportDataTable(UVtaTextureAtlas* TextureAtlas, const FString& TargetPath, const FString& DesiredName, EObjectFlags Flags)
{
	if (TextureAtlas)
	{
		TextureAtlas->Modify();

		TextureAtlas->Table = ExistingTable;
		
		UDataTable* DataTable = nullptr;
		if (TextureAtlas->Table != nullptr)
		{
			DataTable = TextureAtlas->Table;
			DataTable->EmptyTable();
		}
		else
		{
			DataTable = CastChecked<UDataTable>(CreateNewAsset(UDataTable::StaticClass(), TargetPath, DesiredName + TEXT("_DataTable"), Flags));
			DataTable->RowStruct = FVtaAsset::StaticStruct();
			
			TextureAtlas->Table = DataTable;
			TextureAtlas->PostEditChange();
		}
		
		DataTable->Modify();
		
		TSet<FString> ExName;
		
		for (const FString& Name : TextureAtlas->FrameNames)
		{
			FVtaAsset Asset;
			
			Asset.Material = FindMaterialByFrameName(Name, TextureAtlas->Frames);
			Asset.SlateTexture = FindSlateTextureByFrameName(Name, TextureAtlas->SlateTextures);
			
			int32 Position = INDEX_NONE;
			Name.FindLastChar(TEXT('.'), Position);
			
			FString RowName = (Position < 1) ? Name : Name.Left(Position);
			RowName = TEXT("_") + PackageTools::SanitizePackageName(RowName);
			
			while (RowName.FindChar(TEXT('_'), Position))
			{
				RowName.RemoveAt(Position);
				if (Position < RowName.Len())
				{
					RowName.InsertAt(Position, FChar::ToUpper(RowName[Position]));
					RowName.RemoveAt(Position + 1);
				}
			}
			
			FString OriginalRowName = RowName;
			
			int32 i = 0;
			while (ExName.Contains(RowName))
			{
				RowName = FString::Printf(TEXT("%s%d"), *OriginalRowName, ++i);
			}
			
			ExName.Add(RowName);
			DataTable->AddRow(FName(*RowName), Asset);
		}
		
		DataTable->PostEditChange();
	}
	
}

FString UVtaTextureAtlasImportFactory::BuildFrameName(const FString& AtlasName, const FString& FrameName)
{
	return TEXT("MIA_") + AtlasName + TEXT("_") + FrameName;
}

FString UVtaTextureAtlasImportFactory::BuildSlateTextureName(const FString& AtlasName, const FString& FrameName)
{
	return TEXT("ST_") + AtlasName + TEXT("_") + FrameName;
}

//////////////////////////////////////////////////////////////////////////
// Reimport (used by derived class to provide existing data)

void UVtaTextureAtlasImportFactory::SetReimportData(UVtaTextureAtlas* TextureAtlas)
{
	ExistingTable = TextureAtlas->Table;
	
	for (const FString& Name : TextureAtlas->FrameNames)
	{
		UMaterialInstanceConstant* LoadedFrame = FindMaterialByFrameName(Name, TextureAtlas->Frames);
		UVtaSlateTexture* LoadedSlateTexture = FindSlateTextureByFrameName(Name, TextureAtlas->SlateTextures);
		
		if (LoadedFrame != nullptr)
		{
			ExistingFrames.Add(Name, LoadedFrame);
		}
		
		if (LoadedSlateTexture != nullptr)
		{
			ExistingSlateTextures.Add(Name, LoadedSlateTexture);
		}
	}
	
	bIsReimporting = true;
}

void UVtaTextureAtlasImportFactory::ResetImportData()
{
	bIsReimporting = false;

	ExistingAtlasTextureName = FString();
	ExistingAtlasTexture = nullptr;

	ExistingFrames.Empty();
	ExistingSlateTextures.Empty();
	
	ExistingTable = nullptr;
}

UMaterialInstanceConstant* UVtaTextureAtlasImportFactory::FindExistingFrame(const FString& Name)
{
	return ExistingFrames.FindRef(Name);
}

UVtaSlateTexture* UVtaTextureAtlasImportFactory::FindExistingSlateTexture(const FString& Name)
{
	return ExistingSlateTextures.FindRef(Name);
}

UMaterialInstanceConstant* UVtaTextureAtlasImportFactory::FindMaterialByFrameName(const FString& Name, TArray<TAssetPtr<UMaterialInstanceConstant>> List)
{
	FString FindName = PackageTools::SanitizePackageName(Name);
	for (auto AssetPtr : List)
	{
		FString AssetName = AssetPtr.ToSoftObjectPath().GetAssetName();
		int32 Position = AssetName.Find(FindName);
		if (Position != INDEX_NONE)
		{
			return Cast<UMaterialInstanceConstant>(StaticLoadObject(UMaterialInstanceConstant::StaticClass(), nullptr, *AssetPtr.ToSoftObjectPath().ToString(), nullptr, LOAD_None, nullptr));
		}
	}
	
	return nullptr;
}

UVtaSlateTexture* UVtaTextureAtlasImportFactory::FindSlateTextureByFrameName(const FString& Name, TArray<TAssetPtr<UVtaSlateTexture>> List)
{
	FString FindName = PackageTools::SanitizePackageName(Name);
	for (auto AssetPtr : List)
	{
		FString AssetName = AssetPtr.ToSoftObjectPath().GetAssetName();
		int32 Position = AssetName.Find(FindName);
		if (Position != INDEX_NONE)
		{
			return Cast<UVtaSlateTexture>(StaticLoadObject(UVtaSlateTexture::StaticClass(), nullptr, *AssetPtr.ToSoftObjectPath().ToString(), nullptr, LOAD_None, nullptr));
		}
	}
	
	return nullptr;
}



//////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
