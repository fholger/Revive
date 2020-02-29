#pragma once
#include "CompositorBase.h"

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <openvr.h>

class CompositorD3D :
	public CompositorBase
{
public:
	CompositorD3D(ID3D11Device* pDevice);
	CompositorD3D(ID3D12CommandQueue* pQueue);
	virtual ~CompositorD3D();

	static CompositorD3D* Create(IUnknown* d3dPtr);
	virtual vr::ETextureType GetAPI() { return vr::TextureType_DirectX; };
	virtual void Flush() { if (m_pContext) m_pContext->Flush(); };
	virtual TextureBase* CreateTexture();

	virtual void RenderTextureSwapChain(vr::EVREye eye, TextureBase* src, TextureBase* dst, ovrRecti viewport, vr::VRTextureBounds_t bounds, vr::HmdVector4_t quad);
	virtual void RenderMirrorTexture(ovrMirrorTexture mirrorTexture);

	void SetupRenderHiddenAreaMeshToDepthHack();
	void RenderHiddenAreaMeshToDepth();

protected:
	// DirectX 11
	Microsoft::WRL::ComPtr<ID3D11Device> m_pDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pContext;

	// DirectX 12
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_pQueue;

	// Shaders
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_VertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_MirrorShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_CompositorShader;

	// Input
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_VertexBuffer;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_InputLayout;

	// States
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_BlendState;

	// Mirror
	ID3D11ShaderResourceView* m_pMirror[ovrEye_Count];

	// Hidden area mesh resources
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_HiddenAreaMeshVertexBuffer;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_HiddenAreaMeshInputLayout;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_HiddenAreaMeshVertexShader;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_HiddenAreaMeshRasterizerState;
	unsigned int m_HiddenAreaMeshNumVertices;
};
