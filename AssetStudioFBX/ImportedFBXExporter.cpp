#include <fbxsdk.h>
#include <fbxsdk/fileio/fbxiosettings.h>
#include "AssetStudioFBX.h"

namespace AssetStudio
{
	void Fbx::Exporter::Export(String^ path, IImported^ imported, bool EulerFilter, float filterPrecision, String^ exportFormat, bool allFrames, bool allBones, bool skins, float boneSize, bool flatInbetween, bool compatibility)
	{
		FileInfo^ file = gcnew FileInfo(path);
		DirectoryInfo^ dir = file->Directory;
		if (!dir->Exists)
		{
			dir->Create();
		}
		String^ currentDir = Directory::GetCurrentDirectory();
		Directory::SetCurrentDirectory(dir->FullName);
		path = Path::GetFileName(path);

		Exporter^ exporter = gcnew Exporter(path, imported, exportFormat, allFrames, allBones, skins, boneSize, compatibility, true);
		exporter->ExportMorphs(imported, false, flatInbetween);
		exporter->ExportAnimations(EulerFilter, filterPrecision, flatInbetween);
		exporter->pExporter->Export(exporter->pScene);

		Directory::SetCurrentDirectory(currentDir);
	}

	void Fbx::Exporter::ExportMorph(String^ path, IImported^ imported, String^ exportFormat, bool morphMask, bool flatInbetween, bool skins, float boneSize, bool compatibility)
	{
		FileInfo^ file = gcnew FileInfo(path);
		DirectoryInfo^ dir = file->Directory;
		if (!dir->Exists)
		{
			dir->Create();
		}
		String^ currentDir = Directory::GetCurrentDirectory();
		Directory::SetCurrentDirectory(dir->FullName);
		path = Path::GetFileName(path);

		Exporter^ exporter = gcnew Exporter(path, imported, exportFormat, false, true, skins, boneSize, compatibility, false);
		exporter->ExportMorphs(imported, morphMask, flatInbetween);
		exporter->pExporter->Export(exporter->pScene);
		delete exporter;

		Directory::SetCurrentDirectory(currentDir);
	}

	Fbx::Exporter::Exporter(String^ path, IImported^ imported, String^ exportFormat, bool allFrames, bool allBones, bool skins, float boneSize, bool compatibility, bool normals)
	{
		this->imported = imported;
		exportSkins = skins;
		this->boneSize = boneSize;

		cDest = NULL;
		cFormat = NULL;
		pSdkManager = NULL;
		pScene = NULL;
		pExporter = NULL;
		pMaterials = NULL;
		pTextures = NULL;
		pMeshNodes = NULL;

		pin_ptr<FbxManager*> pSdkManagerPin = &pSdkManager;
		pin_ptr<FbxScene*> pScenePin = &pScene;
		Init(pSdkManagerPin, pScenePin);

		cDest = Fbx::StringToCharArray(path);
		cFormat = Fbx::StringToCharArray(exportFormat);
		pExporter = FbxExporter::Create(pScene, "");
		int lFormatIndex, lFormatCount = pSdkManager->GetIOPluginRegistry()->GetWriterFormatCount();
		for (lFormatIndex = 0; lFormatIndex < lFormatCount; lFormatIndex++)
		{
			FbxString lDesc = FbxString(pSdkManager->GetIOPluginRegistry()->GetWriterFormatDescription(lFormatIndex));
			if (lDesc.Find(cFormat) >= 0)
			{
				if (pSdkManager->GetIOPluginRegistry()->WriterIsFBX(lFormatIndex))
				{
					if (lDesc.Find("binary") >= 0)
					{
						if (!compatibility || lDesc.Find("6.") >= 0)
						{
							break;
						}
					}
				}
				else
				{
					break;
				}
			}
		}

		IOS_REF.SetBoolProp(EXP_FBX_MATERIAL, true);
		IOS_REF.SetBoolProp(EXP_FBX_TEXTURE, true);
		IOS_REF.SetBoolProp(EXP_FBX_EMBEDDED, false);
		IOS_REF.SetBoolProp(EXP_FBX_SHAPE, true);
		IOS_REF.SetBoolProp(EXP_FBX_GOBO, true);
		IOS_REF.SetBoolProp(EXP_FBX_ANIMATION, true);
		IOS_REF.SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);

		FbxGlobalSettings& globalSettings = pScene->GetGlobalSettings();
		FbxTime::EMode pTimeMode = FbxTime::eFrames24;
		globalSettings.SetTimeMode(pTimeMode);

		if (!pExporter->Initialize(cDest, lFormatIndex, pSdkManager->GetIOSettings()))
		{
			throw gcnew Exception(gcnew String("Failed to initialize FbxExporter: ") + gcnew String(pExporter->GetStatus().GetErrorString()));
		}

		frameNames = nullptr;
		if (!allFrames)
		{
			frameNames = SearchHierarchy();
			if (!frameNames)
			{
				return;
			}
		}

		pMeshNodes = imported->MeshList != nullptr ? new FbxArray<FbxNode*>(imported->MeshList->Count) : NULL;
		ExportFrame(pScene->GetRootNode(), imported->FrameList[0]);

		if (imported->MeshList != nullptr)
		{
			SetJointsFromImportedMeshes(allBones);

			pMaterials = new FbxArray<FbxSurfacePhong*>();
			pTextures = new FbxArray<FbxFileTexture*>();
			pMaterials->Reserve(imported->MaterialList->Count);
			pTextures->Reserve(imported->TextureList->Count);

			for (int i = 0; i < pMeshNodes->GetCount(); i++)
			{
				FbxNode* meshNode = pMeshNodes->GetAt(i);
				String^ meshPath = gcnew String(meshNode->GetName());
				FbxNode* rootNode = meshNode;
				while ((rootNode = rootNode->GetParent()) != pScene->GetRootNode())
				{
					meshPath = gcnew String(rootNode->GetName()) + "/" + meshPath;
				}
				ImportedMesh^ mesh = ImportedHelpers::FindMesh(meshPath, imported->MeshList);
				ExportMesh(meshNode, mesh, normals);
			}
		}
		else
		{
			SetJointsNode(pScene->GetRootNode()->GetChild(0), nullptr, true);
		}
	}

	HashSet<String^>^ Fbx::Exporter::SearchHierarchy()
	{
		if (imported->MeshList == nullptr || imported->MeshList->Count == 0)
		{
			return nullptr;
		}
		HashSet<String^>^ exportFrames = gcnew HashSet<String^>();
		SearchHierarchy(imported->FrameList[0], exportFrames);
		return exportFrames;
	}

	void Fbx::Exporter::SearchHierarchy(ImportedFrame^ frame, HashSet<String^>^ exportFrames)
	{
		ImportedMesh^ meshListSome = ImportedHelpers::FindMesh(frame, imported->MeshList);
		if (meshListSome != nullptr)
		{
			ImportedFrame^ parent = frame;
			while (parent != nullptr)
			{
				exportFrames->Add(parent->Name);
				parent = (ImportedFrame^)parent->Parent;
			}

			List<ImportedBone^>^ boneList = meshListSome->BoneList;
			if (boneList != nullptr)
			{
				for (int i = 0; i < boneList->Count; i++)
				{
					if (!exportFrames->Contains(boneList[i]->Name))
					{
						ImportedFrame^ boneParent = ImportedHelpers::FindFrame(boneList[i]->Name, imported->FrameList[0]);
						while (boneParent != nullptr)
						{
							exportFrames->Add(boneParent->Name);
							boneParent = (ImportedFrame^)boneParent->Parent;
						}
					}
				}
			}
		}

		for (int i = 0; i < frame->Count; i++)
		{
			SearchHierarchy(frame[i], exportFrames);
		}
	}

	void Fbx::Exporter::SetJointsFromImportedMeshes(bool allBones)
	{
		if (!exportSkins)
		{
			return;
		}
		HashSet<String^>^ boneNames = gcnew HashSet<String^>();
		for (int i = 0; i < imported->MeshList->Count; i++)
		{
			ImportedMesh^ meshList = imported->MeshList[i];
			List<ImportedBone^>^ boneList = meshList->BoneList;
			if (boneList != nullptr)
			{
				for (int j = 0; j < boneList->Count; j++)
				{
					ImportedBone^ bone = boneList[j];
					boneNames->Add(bone->Name);
				}
			}
		}

		SetJointsNode(pScene->GetRootNode()->GetChild(0), boneNames, allBones);
	}

	void Fbx::Exporter::ExportFrame(FbxNode* pParentNode, ImportedFrame^ frame)
	{
		String^ frameName = frame->Name;
		if ((frameNames == nullptr) || frameNames->Contains(frameName))
		{
			FbxNode* pFrameNode = NULL;
			char* pName = NULL;
			try
			{
				pName = StringToCharArray(frameName);
				pFrameNode = FbxNode::Create(pScene, pName);
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pName);
			}

			Vector3 scale, translate;
			Quaternion rotate;
			frame->Matrix.Decompose(scale, rotate, translate);
			Vector3 rotateVector = Fbx::QuaternionToEuler(rotate);

			pFrameNode->LclScaling.Set(FbxVector4(scale.X, scale.Y, scale.Z));
			pFrameNode->LclRotation.Set(FbxVector4(FbxDouble3(rotateVector.X, rotateVector.Y, rotateVector.Z)));
			pFrameNode->LclTranslation.Set(FbxVector4(translate.X, translate.Y, translate.Z));
			pParentNode->AddChild(pFrameNode);

			if (imported->MeshList != nullptr && ImportedHelpers::FindMesh(frame, imported->MeshList) != nullptr)
			{
				pMeshNodes->Add(pFrameNode);
			}

			for (int i = 0; i < frame->Count; i++)
			{
				ExportFrame(pFrameNode, frame[i]);
			}
		}
	}

	void Fbx::Exporter::ExportMesh(FbxNode* pFrameNode, ImportedMesh^ meshList, bool normals)
	{
		int lastSlash = meshList->Name->LastIndexOf('/');
		String^ frameName = lastSlash < 0 ? meshList->Name : meshList->Name->Substring(lastSlash + 1);
		List<ImportedBone^>^ boneList = meshList->BoneList;
		bool hasBones;
		if (exportSkins && boneList != nullptr)
		{
			hasBones = boneList->Count > 0;
		}
		else
		{
			hasBones = false;
		}

		FbxArray<FbxNode*>* pBoneNodeList = NULL;
		try
		{
			if (hasBones)
			{
				pBoneNodeList = new FbxArray<FbxNode*>();
				pBoneNodeList->Reserve(boneList->Count);
				for (int i = 0; i < boneList->Count; i++)
				{
					ImportedBone^ bone = boneList[i];
					String^ boneName = bone->Name;
					char* pBoneName = NULL;
					try
					{
						pBoneName = StringToCharArray(boneName);
						FbxNode* foundNode = pScene->GetRootNode()->FindChild(pBoneName);
						if (foundNode == NULL)
						{
							throw gcnew Exception(gcnew String("Couldn't find frame ") + boneName + gcnew String(" used by the bone"));
						}
						pBoneNodeList->Add(foundNode);
					}
					finally
					{
						Marshal::FreeHGlobal((IntPtr)pBoneName);
					}
				}
			}

			for (int i = 0; i < meshList->SubmeshList->Count; i++)
			{
				char* pName = NULL;
				FbxArray<FbxCluster*>* pClusterArray = NULL;
				try
				{
					pName = StringToCharArray(frameName + "_" + i);
					FbxMesh* pMesh = FbxMesh::Create(pScene, "");

					if (hasBones)
					{
						pClusterArray = new FbxArray<FbxCluster*>();
						pClusterArray->Reserve(boneList->Count);

						for (int i = 0; i < boneList->Count; i++)
						{
							FbxNode* pNode = pBoneNodeList->GetAt(i);
							FbxString lClusterName = pNode->GetNameOnly() + FbxString("Cluster");
							FbxCluster* pCluster = FbxCluster::Create(pSdkManager, lClusterName.Buffer());
							pCluster->SetLink(pNode);
							pCluster->SetLinkMode(FbxCluster::eTotalOne);
							pClusterArray->Add(pCluster);
						}
					}

					ImportedSubmesh^ meshObj = meshList->SubmeshList[i];
					List<ImportedFace^>^ faceList = meshObj->FaceList;
					List<ImportedVertex^>^ vertexList = meshObj->VertexList;

					pMesh->InitControlPoints(vertexList->Count);
					FbxVector4* pControlPoints = pMesh->GetControlPoints();

					FbxGeometryElementNormal* lGeometryElementNormal = NULL;
					//if (normals)
					{
						lGeometryElementNormal = pMesh->GetElementNormal();
						if (!lGeometryElementNormal)
						{
							lGeometryElementNormal = pMesh->CreateElementNormal();
						}
						lGeometryElementNormal->SetMappingMode(FbxGeometryElement::eByControlPoint);
						lGeometryElementNormal->SetReferenceMode(FbxGeometryElement::eDirect);
					}

					FbxGeometryElementUV* lGeometryElementUV = pMesh->GetElementUV();
					if (!lGeometryElementUV)
					{
						lGeometryElementUV = pMesh->CreateElementUV("");
					}
					lGeometryElementUV->SetMappingMode(FbxGeometryElement::eByControlPoint);
					lGeometryElementUV->SetReferenceMode(FbxGeometryElement::eDirect);

					FbxGeometryElementTangent* lGeometryElementTangent = NULL;
					if (normals)
					{
						lGeometryElementTangent = pMesh->GetElementTangent();
						if (!lGeometryElementTangent)
						{
							lGeometryElementTangent = pMesh->CreateElementTangent();
						}
						lGeometryElementTangent->SetMappingMode(FbxGeometryElement::eByControlPoint);
						lGeometryElementTangent->SetReferenceMode(FbxGeometryElement::eDirect);
					}

					bool vertexColours = vertexList->Count > 0 && dynamic_cast<ImportedVertexWithColour^>(vertexList[0]) != nullptr;
					if (vertexColours)
					{
						FbxGeometryElementVertexColor* lGeometryElementVertexColor = pMesh->CreateElementVertexColor();
						lGeometryElementVertexColor->SetMappingMode(FbxGeometryElement::eByControlPoint);
						lGeometryElementVertexColor->SetReferenceMode(FbxGeometryElement::eDirect);
						for (int j = 0; j < vertexList->Count; j++)
						{
							ImportedVertexWithColour^ vert = (ImportedVertexWithColour^)vertexList[j];
							lGeometryElementVertexColor->GetDirectArray().Add(FbxColor(vert->Colour.Red, vert->Colour.Green, vert->Colour.Blue, vert->Colour.Alpha));
						}
					}

					FbxNode* pMeshNode = FbxNode::Create(pScene, pName);
					pMeshNode->SetNodeAttribute(pMesh);
					pFrameNode->AddChild(pMeshNode);

					ImportedMaterial^ mat = ImportedHelpers::FindMaterial(meshObj->Material, imported->MaterialList);
					if (mat != nullptr)
					{
						FbxGeometryElementMaterial* lGeometryElementMaterial = pMesh->GetElementMaterial();
						if (!lGeometryElementMaterial)
						{
							lGeometryElementMaterial = pMesh->CreateElementMaterial();
						}
						lGeometryElementMaterial->SetMappingMode(FbxGeometryElement::eByPolygon);
						lGeometryElementMaterial->SetReferenceMode(FbxGeometryElement::eIndexToDirect);

						char* pMatName = NULL;
						try
						{
							pMatName = StringToCharArray(mat->Name);
							int foundMat = -1;
							for (int j = 0; j < pMaterials->GetCount(); j++)
							{
								FbxSurfacePhong* pMatTemp = pMaterials->GetAt(j);
								if (strcmp(pMatTemp->GetName(), pMatName) == 0)
								{
									foundMat = j;
									break;
								}
							}

							FbxSurfacePhong* pMat;
							if (foundMat >= 0)
							{
								pMat = pMaterials->GetAt(foundMat);
							}
							else
							{
								FbxString lShadingName = "Phong";
								Color4 diffuse = mat->Diffuse;
								Color4 ambient = mat->Ambient;
								Color4 emissive = mat->Emissive;
								Color4 specular = mat->Specular;
								float specularPower = mat->Power;
								pMat = FbxSurfacePhong::Create(pScene, pMatName);
								pMat->Diffuse.Set(FbxDouble3(diffuse.Red, diffuse.Green, diffuse.Blue));
								pMat->DiffuseFactor.Set(FbxDouble(diffuse.Alpha));
								pMat->Ambient.Set(FbxDouble3(ambient.Red, ambient.Green, ambient.Blue));
								pMat->AmbientFactor.Set(FbxDouble(ambient.Alpha));
								pMat->Emissive.Set(FbxDouble3(emissive.Red, emissive.Green, emissive.Blue));
								pMat->EmissiveFactor.Set(FbxDouble(emissive.Alpha));
								pMat->Specular.Set(FbxDouble3(specular.Red, specular.Green, specular.Blue));
								pMat->SpecularFactor.Set(FbxDouble(specular.Alpha));
								pMat->Shininess.Set(specularPower);
								pMat->ShadingModel.Set(lShadingName);

								foundMat = pMaterials->GetCount();
								pMaterials->Add(pMat);
							}
							pMeshNode->AddMaterial(pMat);

							bool hasTexture = false;
							FbxFileTexture* pTextureDiffuse = ExportTexture(ImportedHelpers::FindTexture((String^)mat->Textures[0], imported->TextureList), pMesh);
							if (pTextureDiffuse != NULL)
							{
								LinkTexture(mat, 0, pTextureDiffuse, pMat->Diffuse);
								pMat->TransparentColor.ConnectSrcObject(pTextureDiffuse);
								hasTexture = true;
							}

							FbxFileTexture* pTextureAmbient = ExportTexture(ImportedHelpers::FindTexture((String^)mat->Textures[1], imported->TextureList), pMesh);
							if (pTextureAmbient != NULL)
							{
								LinkTexture(mat, 1, pTextureAmbient, pMat->Ambient);
								hasTexture = true;
							}

							FbxFileTexture* pTextureEmissive = ExportTexture(ImportedHelpers::FindTexture((String^)mat->Textures[2], imported->TextureList), pMesh);
							if (pTextureEmissive != NULL)
							{
								LinkTexture(mat, 2, pTextureEmissive, pMat->Emissive);
								hasTexture = true;
							}

							FbxFileTexture* pTextureSpecular = ExportTexture(ImportedHelpers::FindTexture((String^)mat->Textures[3], imported->TextureList), pMesh);
							if (pTextureSpecular != NULL)
							{
								LinkTexture(mat, 3, pTextureSpecular, pMat->Specular);
								hasTexture = true;
							}

							if (mat->Textures->Length > 4)
							{
								FbxFileTexture* pTextureBump = ExportTexture(ImportedHelpers::FindTexture((String^)mat->Textures[4], imported->TextureList), pMesh);
								if (pTextureBump != NULL)
								{
									LinkTexture(mat, 4, pTextureBump, pMat->Bump);
									hasTexture = true;
								}
							}

							if (hasTexture)
							{
								pMeshNode->SetShadingMode(FbxNode::eTextureShading);
							}
						}
						finally
						{
							Marshal::FreeHGlobal((IntPtr)pMatName);
						}
					}

					for (int j = 0; j < vertexList->Count; j++)
					{
						ImportedVertex^ vertex = vertexList[j];
						Vector3 coords = vertex->Position;
						pControlPoints[j] = FbxVector4(coords.X, coords.Y, coords.Z, 0);
						//if (normals)
						{
							Vector3 normal = vertex->Normal;
							lGeometryElementNormal->GetDirectArray().Add(FbxVector4(normal.X, normal.Y, normal.Z, 0));
						}
						array<float>^ uv = vertex->UV;
						lGeometryElementUV->GetDirectArray().Add(FbxVector2(uv[0], -uv[1]));
						if (normals)
						{
							Vector4 tangent = vertex->Tangent;
							lGeometryElementTangent->GetDirectArray().Add(FbxVector4(tangent.X, tangent.Y, tangent.Z, -tangent.W));
						}

						if (hasBones)
						{
							array<unsigned char>^ boneIndices = vertex->BoneIndices;
							array<float>^ weights4 = vertex->Weights;
							for (int k = 0; k < weights4->Length; k++)
							{
								if (boneIndices[k] < boneList->Count && weights4[k] > 0)
								{
									FbxCluster* pCluster = pClusterArray->GetAt(boneIndices[k]);
									pCluster->AddControlPointIndex(j, weights4[k]);
								}
							}
						}
					}

					for (int j = 0; j < faceList->Count; j++)
					{
						ImportedFace^ face = faceList[j];
						unsigned short v1 = (unsigned short)face->VertexIndices[0];
						unsigned short v2 = (unsigned short)face->VertexIndices[1];
						unsigned short v3 = (unsigned short)face->VertexIndices[2];
						pMesh->BeginPolygon(false);
						pMesh->AddPolygon(v1);
						pMesh->AddPolygon(v2);
						pMesh->AddPolygon(v3);
						pMesh->EndPolygon();
					}

					if (hasBones)
					{
						FbxSkin* pSkin = FbxSkin::Create(pScene, "");
						for (int j = 0; j < boneList->Count; j++)
						{
							FbxCluster* pCluster = pClusterArray->GetAt(j);
							if (pCluster->GetControlPointIndicesCount() > 0)
							{
								FbxNode* pBoneNode = pBoneNodeList->GetAt(j);
								Matrix boneMatrix = boneList[j]->Matrix;
								FbxAMatrix lBoneMatrix;
								for (int m = 0; m < 4; m++)
								{
									for (int n = 0; n < 4; n++)
									{
										lBoneMatrix.mData[m][n] = boneMatrix[m, n];
									}
								}

								FbxAMatrix lMeshMatrix = pMeshNode->EvaluateGlobalTransform();

								pCluster->SetTransformMatrix(lMeshMatrix);
								pCluster->SetTransformLinkMatrix(lMeshMatrix * lBoneMatrix.Inverse());

								pSkin->AddCluster(pCluster);
							}
						}

						if (pSkin->GetClusterCount() > 0)
						{
							pMesh->AddDeformer(pSkin);
						}
					}
				}
				finally
				{
					if (pClusterArray != NULL)
					{
						delete pClusterArray;
					}
				Marshal::FreeHGlobal((IntPtr)pName);
				}
			}
		}
		finally
		{
			if (pBoneNodeList != NULL)
			{
				delete pBoneNodeList;
			}
		}
	}

	FbxFileTexture* Fbx::Exporter::ExportTexture(ImportedTexture^ matTex, FbxMesh* pMesh)
	{
		FbxFileTexture* pTex = NULL;

		if (matTex != nullptr)
		{
			String^ matTexName = matTex->Name;
			char* pTexName = NULL;
			try
			{
				pTexName = StringToCharArray(matTexName);
				int foundTex = -1;
				for (int i = 0; i < pTextures->GetCount(); i++)
				{
					FbxFileTexture* pTexTemp = pTextures->GetAt(i);
					if (strcmp(pTexTemp->GetName(), pTexName) == 0)
					{
						foundTex = i;
						break;
					}
				}

				if (foundTex >= 0)
				{
					pTex = pTextures->GetAt(foundTex);
				}
				else
				{
					pTex = FbxFileTexture::Create(pScene, pTexName);
					pTex->SetFileName(pTexName);
					pTex->SetTextureUse(FbxTexture::eStandard);
					pTex->SetMappingType(FbxTexture::eUV);
					pTex->SetMaterialUse(FbxFileTexture::eModelMaterial);
					pTex->SetSwapUV(false);
					pTex->SetTranslation(0.0, 0.0);
					pTex->SetScale(1.0, 1.0);
					pTex->SetRotation(0.0, 0.0);
					pTextures->Add(pTex);

					String^ path = Path::GetDirectoryName(gcnew String(pExporter->GetFileName().Buffer()));
					if (path == String::Empty)
					{
						path = ".";
					}
					FileInfo^ file = gcnew FileInfo(path + Path::DirectorySeparatorChar + Path::GetFileName(matTex->Name));
					DirectoryInfo^ dir = file->Directory;
					if (!dir->Exists)
					{
						dir->Create();
					}
					BinaryWriter^ writer = gcnew BinaryWriter(file->Create());
					writer->Write(matTex->Data);
					writer->Close();
				}
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pTexName);
			}
		}

		return pTex;
	}

	void Fbx::Exporter::LinkTexture(ImportedMaterial^ mat, int attIndex, FbxFileTexture* pTexture, FbxProperty& prop)
	{
		if (mat->TexOffsets != nullptr)
		{
			pTexture->SetTranslation(mat->TexOffsets[attIndex].X, mat->TexOffsets[attIndex].Y);
		}
		if (mat->TexScales != nullptr)
		{
			pTexture->SetScale(mat->TexScales[attIndex].X, mat->TexScales[attIndex].Y);
		}
		prop.ConnectSrcObject(pTexture);
	}

	void Fbx::Exporter::ExportAnimations(bool EulerFilter, float filterPrecision, bool flatInbetween)
	{
		List<ImportedAnimation^>^ importedAnimationList = imported->AnimationList;
		if (importedAnimationList == nullptr)
		{
			return;
		}

		List<String^>^ pNotFound = gcnew List<String^>();

		FbxPropertyT<FbxDouble3> scale = FbxProperty::Create(pScene, FbxDouble3DT, InterpolationHelper::pScaleName);
		FbxPropertyT<FbxDouble4> rotate = FbxProperty::Create(pScene, FbxDouble4DT, InterpolationHelper::pRotateName);
		FbxPropertyT<FbxDouble3> translate = FbxProperty::Create(pScene, FbxDouble3DT, InterpolationHelper::pTranslateName);

		FbxAnimCurveFilterUnroll* lFilter = EulerFilter ? new FbxAnimCurveFilterUnroll() : NULL;

		for (int i = 0; i < importedAnimationList->Count; i++)
		{
			bool keyframed = dynamic_cast<ImportedKeyframedAnimation^>(importedAnimationList[i]) != nullptr;
			if (keyframed)
			{
				ImportedKeyframedAnimation^ parser = (ImportedKeyframedAnimation^)importedAnimationList[i];
				FbxString kTakeName;
				if (parser->Name)
				{
					WITH_MARSHALLED_STRING
					(
						pClipName,
						parser->Name,
						kTakeName = FbxString(pClipName);
					);
				}
				else
				{
					kTakeName = FbxString("Take") + FbxString(i);
				}
				ExportKeyframedAnimation(parser, kTakeName, lFilter, filterPrecision, scale, rotate, translate, pNotFound);
			}
			else
			{
				ImportedSampledAnimation^ parser = (ImportedSampledAnimation^)importedAnimationList[i];
				FbxString kTakeName;
				if (parser->Name)
				{
					WITH_MARSHALLED_STRING
					(
						pClipName,
						parser->Name,
						kTakeName = FbxString(pClipName);
					);
				}
				else
				{
					kTakeName = FbxString("Take") + FbxString(i);
				}
				ExportSampledAnimation(parser, kTakeName, lFilter, filterPrecision, flatInbetween, scale, rotate, translate, pNotFound);
			}
		}

		if (pNotFound->Count > 0)
		{
			String^ pNotFoundString = gcnew String("Warning: Animations weren't exported for the following missing frames or morphs: ");
			for (int i = 0; i < pNotFound->Count; i++)
			{
				pNotFoundString += pNotFound[i] + ", ";
			}
			//Report::ReportLog(pNotFoundString->Substring(0, pNotFoundString->Length - 2));
		}
	}

	void Fbx::Exporter::ExportKeyframedAnimation(ImportedKeyframedAnimation^ parser, FbxString& kTakeName, FbxAnimCurveFilterUnroll* EulerFilter, float filterPrecision,
		FbxPropertyT<FbxDouble3>& scale, FbxPropertyT<FbxDouble4>& rotate, FbxPropertyT<FbxDouble3>& translate, List<String^>^ pNotFound)
	{
		List<ImportedAnimationKeyframedTrack^>^ pAnimationList = parser->TrackList;

		char* lTakeName = kTakeName.Buffer();

		FbxTime lTime;
		FbxAnimStack* lAnimStack = FbxAnimStack::Create(pScene, lTakeName);
		FbxAnimLayer* lAnimLayer = FbxAnimLayer::Create(pScene, "Base Layer");
		lAnimStack->AddMember(lAnimLayer);

		for (int j = 0; j < pAnimationList->Count; j++)
		{
			ImportedAnimationKeyframedTrack^ keyframeList = pAnimationList[j];
			String^ name = keyframeList->Name;
			int dotPos = name->IndexOf('.');
			if (dotPos >= 0 && !ImportedHelpers::FindFrame(name, imported->FrameList[0]))
			{
				name = name->Substring(0, dotPos);
			}
			FbxNode* pNode = NULL;
			char* pName = NULL;
			try
			{
				pName = Fbx::StringToCharArray(name);
				pNode = pScene->GetRootNode()->FindChild(pName);
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pName);
			}

				if (pNode == NULL)
				{
					if (!pNotFound->Contains(name))
					{
						pNotFound->Add(name);
					}
				}
				else
				{
					FbxAnimCurve* lCurveSX = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
					FbxAnimCurve* lCurveSY = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
					FbxAnimCurve* lCurveSZ = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
					FbxAnimCurve* lCurveRX = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
					FbxAnimCurve* lCurveRY = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
					FbxAnimCurve* lCurveRZ = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
					FbxAnimCurve* lCurveTX = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
					FbxAnimCurve* lCurveTY = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
					FbxAnimCurve* lCurveTZ = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);

					lCurveSX->KeyModifyBegin();
					lCurveSY->KeyModifyBegin();
					lCurveSZ->KeyModifyBegin();
					lCurveRX->KeyModifyBegin();
					lCurveRY->KeyModifyBegin();
					lCurveRZ->KeyModifyBegin();
					lCurveTX->KeyModifyBegin();
					lCurveTY->KeyModifyBegin();
					lCurveTZ->KeyModifyBegin();

					for each (auto keyframes in keyframeList->Keyframes)
					{
						lTime.SetSecondDouble(keyframes.Key);

						if (keyframes.Value->Scaling != nullptr)
						{
							lCurveSX->KeySet(lCurveSX->KeyAdd(lTime), lTime, keyframes.Value->Scaling->value.X);
							lCurveSY->KeySet(lCurveSY->KeyAdd(lTime), lTime, keyframes.Value->Scaling->value.Y);
							lCurveSZ->KeySet(lCurveSZ->KeyAdd(lTime), lTime, keyframes.Value->Scaling->value.Z);
						}
						if (keyframes.Value->Rotation != nullptr)
						{
							Vector3 rotation = Fbx::QuaternionToEuler(keyframes.Value->Rotation->value);
							Vector3 inSlope = Fbx::QuaternionToEuler(keyframes.Value->Rotation->inSlope);
							Vector3 outSlope = Fbx::QuaternionToEuler(keyframes.Value->Rotation->outSlope);
							lCurveRX->KeySet(lCurveRX->KeyAdd(lTime), lTime, rotation.X);
							lCurveRY->KeySet(lCurveRY->KeyAdd(lTime), lTime, rotation.Y);
							lCurveRZ->KeySet(lCurveRZ->KeyAdd(lTime), lTime, rotation.Z);
						}
						if (keyframes.Value->Translation != nullptr)
						{
							lCurveTX->KeySet(lCurveTX->KeyAdd(lTime), lTime, keyframes.Value->Translation->value.X);
							lCurveTY->KeySet(lCurveTY->KeyAdd(lTime), lTime, keyframes.Value->Translation->value.Y);
							lCurveTZ->KeySet(lCurveTZ->KeyAdd(lTime), lTime, keyframes.Value->Translation->value.Z);
						}
					}

					lCurveSX->KeyModifyEnd();
					lCurveSY->KeyModifyEnd();
					lCurveSZ->KeyModifyEnd();
					lCurveRX->KeyModifyEnd();
					lCurveRY->KeyModifyEnd();
					lCurveRZ->KeyModifyEnd();
					lCurveTX->KeyModifyEnd();
					lCurveTY->KeyModifyEnd();
					lCurveTZ->KeyModifyEnd();

					if (EulerFilter)
					{
						FbxAnimCurve* lCurve[3];
						lCurve[0] = lCurveRX;
						lCurve[1] = lCurveRY;
						lCurve[2] = lCurveRZ;
						EulerFilter->Reset();
						EulerFilter->SetTestForPath(true);
						EulerFilter->SetQualityTolerance(filterPrecision);
						EulerFilter->Apply(lCurve, 3);
					}
				}
		}
	}

	void Fbx::Exporter::ExportSampledAnimation(ImportedSampledAnimation^ parser, FbxString& kTakeName, FbxAnimCurveFilterUnroll* EulerFilter, float filterPrecision, bool flatInbetween,
		FbxPropertyT<FbxDouble3>& scale, FbxPropertyT<FbxDouble4>& rotate, FbxPropertyT<FbxDouble3>& translate, List<String^>^ pNotFound)
	{
		List<ImportedAnimationSampledTrack^>^ pAnimationList = parser->TrackList;

		char* lTakeName = kTakeName.Buffer();

		FbxTime lTime;
		FbxAnimStack* lAnimStack = FbxAnimStack::Create(pScene, lTakeName);
		FbxAnimLayer* lAnimLayer = FbxAnimLayer::Create(pScene, "Base Layer");
		lAnimStack->AddMember(lAnimLayer);

		const double fps = 1.0 / parser->SampleRate;

		for (int j = 0; j < pAnimationList->Count; j++)
		{
			ImportedAnimationSampledTrack^ sampleList = pAnimationList[j];

			int endAt;
			if (sampleList->Scalings && sampleList->Scalings->Length > 0)
			{
				endAt = sampleList->Scalings->Length;
			}
			else if (sampleList->Rotations && sampleList->Rotations->Length > 0)
			{
				endAt = sampleList->Rotations->Length;
			}
			else if (sampleList->Translations && sampleList->Translations->Length > 0)
			{
				endAt = sampleList->Translations->Length;
			}
			else if (sampleList->Curve && sampleList->Curve->Length > 0)
			{
				endAt = sampleList->Curve->Length;
			}

			String^ name = sampleList->Name;
			int dotPos = name->IndexOf('.');
			if (dotPos >= 0 && !ImportedHelpers::FindFrame(name, imported->FrameList[0]))
			{
				name = name->Substring(0, dotPos);
			}
			FbxNode* pNode = NULL;
			char* pName = NULL;
			try
			{
				pName = Fbx::StringToCharArray(name);
				pNode = pScene->GetRootNode()->FindChild(pName);
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pName);
			}

				if (pNode == NULL)
				{
					if (!pNotFound->Contains(name))
					{
						pNotFound->Add(name);
					}
				}
				else
				{
					if (sampleList->Scalings)
					{
						FbxAnimCurve* lCurveSX = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
						FbxAnimCurve* lCurveSY = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
						FbxAnimCurve* lCurveSZ = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
						lCurveSX->KeyModifyBegin();
						lCurveSY->KeyModifyBegin();
						lCurveSZ->KeyModifyBegin();
						for (int k = 0; k < endAt; k++)
						{
							if (!sampleList->Scalings[k].HasValue)
								continue;

							lTime.SetSecondDouble(fps * k);

							lCurveSX->KeySet(lCurveSX->KeyAdd(lTime), lTime, sampleList->Scalings[k].Value.X);
							lCurveSY->KeySet(lCurveSY->KeyAdd(lTime), lTime, sampleList->Scalings[k].Value.Y);
							lCurveSZ->KeySet(lCurveSZ->KeyAdd(lTime), lTime, sampleList->Scalings[k].Value.Z);
						}
						lCurveSX->KeyModifyEnd();
						lCurveSY->KeyModifyEnd();
						lCurveSZ->KeyModifyEnd();
					}

					if (sampleList->Rotations)
					{
						FbxAnimCurve* lCurveRX = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
						FbxAnimCurve* lCurveRY = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
						FbxAnimCurve* lCurveRZ = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
						lCurveRX->KeyModifyBegin();
						lCurveRY->KeyModifyBegin();
						lCurveRZ->KeyModifyBegin();
						for (int k = 0; k < endAt; k++)
						{
							if (!sampleList->Rotations[k].HasValue)
								continue;

							lTime.SetSecondDouble(fps * k);

							Vector3 rotation = Fbx::QuaternionToEuler(sampleList->Rotations[k].Value);
							lCurveRX->KeySet(lCurveRX->KeyAdd(lTime), lTime, rotation.X);
							lCurveRY->KeySet(lCurveRY->KeyAdd(lTime), lTime, rotation.Y);
							lCurveRZ->KeySet(lCurveRZ->KeyAdd(lTime), lTime, rotation.Z);
						}
						lCurveRX->KeyModifyEnd();
						lCurveRY->KeyModifyEnd();
						lCurveRZ->KeyModifyEnd();

						if (EulerFilter)
						{
							FbxAnimCurve* lCurve[3];
							lCurve[0] = lCurveRX;
							lCurve[1] = lCurveRY;
							lCurve[2] = lCurveRZ;
							EulerFilter->Reset();
							EulerFilter->SetTestForPath(true);
							EulerFilter->SetQualityTolerance(filterPrecision);
							EulerFilter->Apply(lCurve, 3);
						}
					}

					if (sampleList->Translations)
					{
						FbxAnimCurve* lCurveTX = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
						FbxAnimCurve* lCurveTY = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
						FbxAnimCurve* lCurveTZ = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
						lCurveTX->KeyModifyBegin();
						lCurveTY->KeyModifyBegin();
						lCurveTZ->KeyModifyBegin();
						for (int k = 0; k < endAt; k++)
						{
							if (!sampleList->Translations[k].HasValue)
								continue;

							lTime.SetSecondDouble(fps * k);

							lCurveTX->KeySet(lCurveTX->KeyAdd(lTime), lTime, sampleList->Translations[k].Value.X);
							lCurveTY->KeySet(lCurveTY->KeyAdd(lTime), lTime, sampleList->Translations[k].Value.Y);
							lCurveTZ->KeySet(lCurveTZ->KeyAdd(lTime), lTime, sampleList->Translations[k].Value.Z);
						}
						lCurveTX->KeyModifyEnd();
						lCurveTY->KeyModifyEnd();
						lCurveTZ->KeyModifyEnd();
					}

					if (sampleList->Curve)
					{
						FbxNode* pMeshNode = pNode->GetChild(0);
						FbxMesh* pMesh = pMeshNode ? pMeshNode->GetMesh() : NULL;
						if (pMesh)
						{
							name = sampleList->Name->Substring(dotPos + 1);
							int numBlendShapes = pMesh->GetDeformerCount(FbxDeformer::eBlendShape);
							for (int bsIdx = 0; bsIdx < numBlendShapes; bsIdx++)
							{
								FbxBlendShape* lBlendShape = (FbxBlendShape*)pMesh->GetDeformer(bsIdx, FbxDeformer::eBlendShape);
								int numChannels = lBlendShape->GetBlendShapeChannelCount();
								float flatMinStrength = 0, flatMaxStrength;
								String^ shapeName = nullptr;
								for (int chnIdx = 0; chnIdx < numChannels; chnIdx++)
								{
									FbxBlendShapeChannel* lChannel = lBlendShape->GetBlendShapeChannel(chnIdx);
									String^ keyframeName;
									if (!flatInbetween)
									{
										keyframeName = gcnew String(lChannel->GetName());
									}
									else
									{
										shapeName = gcnew String(lChannel->GetTargetShape(0)->GetName());
										keyframeName = shapeName->Substring(0, shapeName->LastIndexOf("_"));
									}
									if (keyframeName == name)
									{
										FbxAnimCurve* lCurve = lChannel->DeformPercent.GetCurve(lAnimLayer, true);
										if (flatInbetween)
										{
											FbxProperty weightProp;
											WITH_MARSHALLED_STRING
											(
												weightName,
												shapeName + ".Weight",
												weightProp = pMesh->FindProperty(weightName);
											);
											if (weightProp.IsValid())
											{
												flatMaxStrength = (float)weightProp.Get<double>();
											}
											else
											{
												flatMaxStrength = 100;
												//Report::ReportLog("Error! Weight for flat Blend-Shape " + shapeName + " not found! Using a value of " + flatMaxStrength);
											}
										}
										lCurve->KeyModifyBegin();
										for (int k = 0; k < endAt; k++)
										{
											if (!sampleList->Curve[k].HasValue)
											{
												continue;
											}

											lTime.SetSecondDouble(fps * k);

											auto keySetIndex = lCurve->KeyAdd(lTime);

											if (!flatInbetween)
											{
												lCurve->KeySet(keySetIndex, lTime, sampleList->Curve[k].Value);
											}
											else
											{
												float val = sampleList->Curve[k].Value;
												if (val >= flatMinStrength && val <= flatMaxStrength)
												{
													val = (val - flatMinStrength) * 100 / (flatMaxStrength - flatMinStrength);
												}
												else if (val < flatMinStrength)
												{
													val = 0;
												}
												else if (val > flatMaxStrength)
												{
													val = 100;
												}
												lCurve->KeySet(keySetIndex, lTime, val);
											}
										}
										lCurve->KeyModifyEnd();
										if (!flatInbetween)
										{
											bsIdx = numBlendShapes;
											break;
										}
										else
										{
											flatMinStrength = flatMaxStrength;
										}
									}
								}
							}
						}
						else
						{
							name = sampleList->Name;
							if (!pNotFound->Contains(name))
							{
								pNotFound->Add(name);
							}
						}
					}
				}
		}
	}

	void Fbx::Exporter::ExportMorphs(IImported^ imported, bool morphMask, bool flatInbetween)
	{
		if (imported->MeshList == nullptr)
		{
			return;
		}

		for (int meshIdx = 0; meshIdx < imported->MeshList->Count; meshIdx++)
		{
			ImportedMesh^ meshList = imported->MeshList[meshIdx];
			FbxNode* pBaseNode = NULL;
			for (int nodeIdx = 0; nodeIdx < pMeshNodes->GetCount(); nodeIdx++)
			{
				FbxNode* pMeshNode = pMeshNodes->GetAt(nodeIdx);
				String^ framePath = gcnew String(pMeshNode->GetName());
				FbxNode* rootNode = pMeshNode;
				while ((rootNode = rootNode->GetParent()) != pScene->GetRootNode())
				{
					framePath = gcnew String(rootNode->GetName()) + "/" + framePath;
				}
				if (framePath == meshList->Name)
				{
					pBaseNode = pMeshNode;
					break;
				}
			}
			if (pBaseNode == NULL)
			{
				continue;
			}

			for each (ImportedMorph^ morph in imported->MorphList)
			{
				if (morph->Name != meshList->Name)
				{
					continue;
				}

				int meshVertexIndex = 0;
				for (int meshObjIdx = 0; meshObjIdx < meshList->SubmeshList->Count; meshObjIdx++)
				{
					List<ImportedVertex^>^ vertList = meshList->SubmeshList[meshObjIdx]->VertexList;
					FbxNode* pBaseMeshNode = pBaseNode->GetChild(meshObjIdx);
					FbxMesh* pBaseMesh = pBaseMeshNode->GetMesh();

					FbxBlendShape* lBlendShape;
					WITH_MARSHALLED_STRING
					(
						pShapeName,
						morph->ClipName + (meshList->SubmeshList->Count > 1 ? "_" + meshObjIdx : String::Empty) /*+ "_BlendShape"*/,
						lBlendShape = FbxBlendShape::Create(pScene, pShapeName);
					);
					pBaseMesh->AddDeformer(lBlendShape);
					List<ImportedMorphKeyframe^>^ keyframes = morph->KeyframeList;
					for (int i = 0; i < morph->Channels->Count; i++)
					{
						FbxBlendShapeChannel* lBlendShapeChannel;
						if (!flatInbetween)
						{
							WITH_MARSHALLED_STRING
							(
								pChannelName,
								gcnew String(lBlendShape->GetName()) + "." + keyframes[morph->Channels[i]->Item2]->Name->Substring(0, keyframes[morph->Channels[i]->Item2]->Name->LastIndexOf("_")),
								lBlendShapeChannel = FbxBlendShapeChannel::Create(pScene, pChannelName);
							);
							lBlendShapeChannel->DeformPercent = morph->Channels[i]->Item1;
							lBlendShape->AddBlendShapeChannel(lBlendShapeChannel);
						}

						for (int frameIdx = 0; frameIdx < morph->Channels[i]->Item3; frameIdx++)
						{
							int shapeIdx = morph->Channels[i]->Item2 + frameIdx;
							ImportedMorphKeyframe^ keyframe = keyframes[shapeIdx];

							FbxShape* pShape;
							if (!flatInbetween)
							{
								WITH_MARSHALLED_STRING
								(
									pMorphShapeName,
									keyframe->Name,
									pShape = FbxShape::Create(pScene, pMorphShapeName);
								);
								lBlendShapeChannel->AddTargetShape(pShape, keyframe->Weight);
							}
							else
							{
								lBlendShapeChannel = FbxBlendShapeChannel::Create(pScene, "");
								lBlendShapeChannel->DeformPercent = morph->Channels[i]->Item1;
								lBlendShape->AddBlendShapeChannel(lBlendShapeChannel);

								WITH_MARSHALLED_STRING
								(
									pMorphShapeName,
									morph->ClipName + (meshList->SubmeshList->Count > 1 ? "_" + meshObjIdx : String::Empty) + "." + keyframe->Name,
									pShape = FbxShape::Create(pScene, pMorphShapeName);
								);
								lBlendShapeChannel->AddTargetShape(pShape, 100);

								FbxProperty weightProp;
								WITH_MARSHALLED_STRING
								(
									pWeightName,
									gcnew String(pShape->GetName()) + ".Weight",
									weightProp = FbxProperty::Create(pBaseMesh, FbxDoubleDT, pWeightName);
								);
								weightProp.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
								weightProp.Set<double>(keyframe->Weight);
							}

							pShape->InitControlPoints(vertList->Count);
							FbxVector4* pControlPoints = pShape->GetControlPoints();

							for (int j = 0; j < vertList->Count; j++)
							{
								ImportedVertex^ vertex = vertList[j];
								Vector3 coords = vertex->Position;
								pControlPoints[j] = FbxVector4(coords.X, coords.Y, coords.Z, 0);
							}
							List<unsigned short>^ meshIndices = keyframe->MorphedVertexIndices;
							for (int j = 0; j < meshIndices->Count; j++)
							{
								int controlPointIndex = meshIndices[j] - meshVertexIndex;
								if (controlPointIndex >= 0 && controlPointIndex < vertList->Count)
								{
									Vector3 coords = keyframe->VertexList[j]->Position;
									pControlPoints[controlPointIndex] = FbxVector4(coords.X, coords.Y, coords.Z, 0);
								}
							}

							if (flatInbetween && frameIdx > 0)
							{
								int shapeIdx = morph->Channels[i]->Item2 + frameIdx - 1;
								ImportedMorphKeyframe^ keyframe = keyframes[shapeIdx];

								List<unsigned short>^ meshIndices = keyframe->MorphedVertexIndices;
								for (int j = 0; j < meshIndices->Count; j++)
								{
									int controlPointIndex = meshIndices[j] - meshVertexIndex;
									if (controlPointIndex >= 0 && controlPointIndex < vertList->Count)
									{
										Vector3 coords = keyframe->VertexList[j]->Position - vertList[controlPointIndex]->Position;
										pControlPoints[controlPointIndex] -= FbxVector4(coords.X, coords.Y, coords.Z, 0);
									}
								}
							}

							if (morphMask)
							{
								FbxGeometryElementVertexColor* lGeometryElementVertexColor = pBaseMesh->CreateElementVertexColor();
								lGeometryElementVertexColor->SetMappingMode(FbxGeometryElement::eByControlPoint);
								lGeometryElementVertexColor->SetReferenceMode(FbxGeometryElement::eDirect);
								WITH_MARSHALLED_STRING
								(
									pColourLayerName, morph->KeyframeList[shapeIdx]->Name,
									lGeometryElementVertexColor->SetName(pColourLayerName);
								);
								for (int j = 0; j < vertList->Count; j++)
								{
									lGeometryElementVertexColor->GetDirectArray().Add(FbxColor(1, 1, 1));
								}
								for (int j = 0; j < meshIndices->Count; j++)
								{
									int controlPointIndex = meshIndices[j] - meshVertexIndex;
									if (controlPointIndex >= 0 && controlPointIndex < vertList->Count)
									{
										lGeometryElementVertexColor->GetDirectArray().SetAt(controlPointIndex, FbxColor(0, 0, 1));
									}
								}
							}
						}
					}
					meshVertexIndex += meshList->SubmeshList[meshObjIdx]->VertexList->Count;
				}
			}
		}
	}
}
