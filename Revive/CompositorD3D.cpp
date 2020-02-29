#include "CompositorD3D.h"
#include "TextureD3D.h"
#include "Session.h"

#include <openvr.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "VertexShader.hlsl.h"
#include "MirrorShader.hlsl.h"
#include "CompositorShader.hlsl.h"
#include "HiddenAreaMeshVertexShader.hlsl.h"
#include "MinHook.h"
#include <fstream>

CompositorD3D* pCompositor = nullptr;

typedef HRESULT(__stdcall* D3D11_ClearDepthStencilView_t)(ID3D11DeviceContext* context, ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
D3D11_ClearDepthStencilView_t pD3D11_ClearDepthStencilView = NULL;

HRESULT __stdcall D3D11_ClearDepthStencilView_Hook(ID3D11DeviceContext* context, ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
	HRESULT ret = pD3D11_ClearDepthStencilView(context, pDepthStencilView, ClearFlags, Depth, Stencil);	
	if (Depth == 0)
	{
		pCompositor->RenderHiddenAreaMeshToDepth();
	}

	return ret;
}

struct Vertex
{
	ovrVector2f Position;
	ovrVector2f TexCoord;
};

CompositorD3D* CompositorD3D::Create(IUnknown* d3dPtr)
{
	// Get the device for this context
	// TODO: DX12 support
	ID3D11Device* pDevice = nullptr;
	HRESULT hr = d3dPtr->QueryInterface(&pDevice);
	if (SUCCEEDED(hr))
		return new CompositorD3D(pDevice);

	ID3D12CommandQueue* pQueue = nullptr;
	hr = d3dPtr->QueryInterface(&pQueue);
	if (SUCCEEDED(hr))
		return new CompositorD3D(pQueue);

	return nullptr;
}

CompositorD3D::CompositorD3D(ID3D11Device* pDevice)
{
	m_pDevice = pDevice;
	m_pDevice->GetImmediateContext(m_pContext.GetAddressOf());

	// Create the shaders.
	m_pDevice->CreateVertexShader(g_VertexShader, sizeof(g_VertexShader), NULL, m_VertexShader.GetAddressOf());
	m_pDevice->CreatePixelShader(g_MirrorShader, sizeof(g_MirrorShader), NULL, m_MirrorShader.GetAddressOf());
	m_pDevice->CreatePixelShader(g_CompositorShader, sizeof(g_CompositorShader), NULL, m_CompositorShader.GetAddressOf());

	// Create the vertex buffer.
	D3D11_BUFFER_DESC bufferDesc;
	bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	bufferDesc.ByteWidth = sizeof(Vertex) * 4;
	bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	bufferDesc.MiscFlags = 0;
	m_pDevice->CreateBuffer(&bufferDesc, nullptr, m_VertexBuffer.GetAddressOf());

	// Create the input layout.
	D3D11_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
		D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
		D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	HRESULT hr = m_pDevice->CreateInputLayout(layout, 2, g_VertexShader, sizeof(g_VertexShader), m_InputLayout.GetAddressOf());

	// Create state objects.
	D3D11_BLEND_DESC bm = { 0 };
	bm.RenderTarget[0].BlendEnable = true;
	bm.RenderTarget[0].BlendOp = bm.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bm.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	bm.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bm.RenderTarget[0].SrcBlendAlpha = bm.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bm.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	m_pDevice->CreateBlendState(&bm, m_BlendState.GetAddressOf());

	// Get the mirror textures
	vr::VRCompositor()->GetMirrorTextureD3D11(vr::Eye_Left, m_pDevice.Get(), (void**)&m_pMirror[ovrEye_Left]);
	vr::VRCompositor()->GetMirrorTextureD3D11(vr::Eye_Right, m_pDevice.Get(), (void**)&m_pMirror[ovrEye_Right]);
}

CompositorD3D::CompositorD3D(ID3D12CommandQueue* pQueue)
	: m_pQueue(pQueue)
	, m_pMirror()
{
}

CompositorD3D::~CompositorD3D()
{
	if (m_pMirror[ovrEye_Left])
		vr::VRCompositor()->ReleaseMirrorTextureD3D11(m_pMirror[ovrEye_Left]);
	if (m_pMirror[ovrEye_Right])
		vr::VRCompositor()->ReleaseMirrorTextureD3D11(m_pMirror[ovrEye_Right]);
}

TextureBase* CompositorD3D::CreateTexture()
{
	if (m_pDevice)
		return new TextureD3D(m_pDevice.Get());
	else
		return new TextureD3D(m_pQueue.Get());
}

void CompositorD3D::RenderMirrorTexture(ovrMirrorTexture mirrorTexture)
{
	// TODO: Support mirror textures in DX12
	if (!m_pDevice)
		return;

	// Get the current state objects
	Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
	float blend_factor[4];
	uint32_t sample_mask;
	m_pContext->OMGetBlendState(blend_state.GetAddressOf(), blend_factor, &sample_mask);

	D3D11_PRIMITIVE_TOPOLOGY topology;
	m_pContext->IAGetPrimitiveTopology(&topology);

	ID3D11RasterizerState* ras_state;
	m_pContext->RSGetState(&ras_state);

	// Get the mirror texture
	TextureD3D* texture = (TextureD3D*)mirrorTexture->Texture.get();

	// Set the mirror shaders
	m_pContext->VSSetShader(m_VertexShader.Get(), NULL, 0);
	m_pContext->PSSetShader(m_MirrorShader.Get(), NULL, 0);
	m_pContext->PSSetShaderResources(0, ovrEye_Count, m_pMirror);

	// Update the vertex buffer
	Vertex vertices[4] = {
		{ { -1.0f,  1.0f },{ 0.0f, 0.0f } },
		{ {  1.0f,  1.0f },{ 1.0f, 0.0f } },
		{ { -1.0f, -1.0f },{ 0.0f, 1.0f } },
		{ {  1.0f, -1.0f },{ 1.0f, 1.0f } }
	};
	D3D11_MAPPED_SUBRESOURCE map = { 0 };
	m_pContext->Map(m_VertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	memcpy(map.pData, vertices, sizeof(Vertex) * 4);
	m_pContext->Unmap(m_VertexBuffer.Get(), 0);

	// Prepare the render target
	FLOAT clear[4] = { 0.0f };
	D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)mirrorTexture->Desc.Width, (float)mirrorTexture->Desc.Height, D3D11_MIN_DEPTH, D3D11_MIN_DEPTH };
	m_pContext->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* target = texture->Target();
	m_pContext->ClearRenderTargetView(target, clear);
	m_pContext->OMSetRenderTargets(1, &target, NULL);
	m_pContext->OMSetBlendState(nullptr, nullptr, -1);
	m_pContext->RSSetState(nullptr);

	// Set and draw the vertices
	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_pContext->IASetInputLayout(m_InputLayout.Get());
	m_pContext->IASetVertexBuffers(0, 1, m_VertexBuffer.GetAddressOf(), &stride, &offset);
	m_pContext->Draw(4, 0);

	// Restore the state objects
	m_pContext->RSSetState(ras_state);
	m_pContext->OMSetBlendState(blend_state.Get(), blend_factor, sample_mask);
	m_pContext->IASetPrimitiveTopology(topology);
}

void CompositorD3D::SetupRenderHiddenAreaMeshToDepthHack()
{
	m_pDevice->CreateVertexShader(g_HiddenAreaMeshVertexShader, sizeof(g_HiddenAreaMeshVertexShader), NULL, m_HiddenAreaMeshVertexShader.GetAddressOf());

	// Create the vertex buffer.
	vr::HiddenAreaMesh_t hamRight = vr::VRSystem()->GetHiddenAreaMesh(vr::Eye_Right);
	vr::HiddenAreaMesh_t hamLeft = vr::VRSystem()->GetHiddenAreaMesh(vr::Eye_Left);
	std::vector<vr::HmdVector2_t> combinedHam (3 * (hamRight.unTriangleCount + hamLeft.unTriangleCount));
	memcpy(&combinedHam[0], hamRight.pVertexData, sizeof(vr::HmdVector2_t) * 3 * hamRight.unTriangleCount);
	memcpy(&combinedHam[3 * hamRight.unTriangleCount], hamLeft.pVertexData, sizeof(vr::HmdVector2_t) * 3 * hamLeft.unTriangleCount);
	for (unsigned int i = 0; i < 3 * (hamRight.unTriangleCount + hamLeft.unTriangleCount); ++i)
	{
		// coordinates from SteamVR are in (0, 1) range, scale y to (-1, 1)
		combinedHam[i].v[1] = combinedHam[i].v[1] * 2 - 1;
		if (i >= 3 * hamRight.unTriangleCount)
		{
			// left-shift the left eye vertices so that they render to the left part of the Stormland render texture
			combinedHam[i].v[0] -= 1;
		}
	}
	m_HiddenAreaMeshNumVertices = combinedHam.size();

	D3D11_BUFFER_DESC bufferDesc;
	bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
	bufferDesc.ByteWidth = sizeof(vr::HmdVector2_t) * combinedHam.size();
	bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bufferDesc.CPUAccessFlags = 0;
	bufferDesc.MiscFlags = 0;
	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem = combinedHam.data();
	m_pDevice->CreateBuffer(&bufferDesc, &data, m_HiddenAreaMeshVertexBuffer.GetAddressOf());

	// Create the input layout.
	D3D11_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
		D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	m_pDevice->CreateInputLayout(layout, 1, g_HiddenAreaMeshVertexShader, sizeof(g_HiddenAreaMeshVertexShader), m_HiddenAreaMeshInputLayout.GetAddressOf());

	// Create the rasterizer state
	D3D11_RASTERIZER_DESC rasterizerDesc;
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	m_pDevice->CreateRasterizerState(&rasterizerDesc, m_HiddenAreaMeshRasterizerState.GetAddressOf());

	// hook ClearDepthStencil
	LPVOID pTarget;
	MH_CreateHookVirtualEx(m_pContext.Get(), 53, &D3D11_ClearDepthStencilView_Hook, (void**)&pD3D11_ClearDepthStencilView, &pTarget);
	MH_EnableHook(pTarget);
	pCompositor = this;
}

void CompositorD3D::RenderHiddenAreaMeshToDepth()
{
	ID3D11RasterizerState* currentState;
	m_pContext->RSGetState(&currentState);

	m_pContext->VSSetShader(m_HiddenAreaMeshVertexShader.Get(), NULL, 0);
	m_pContext->PSSetShader(NULL, NULL, 0);
	m_pContext->RSSetState(m_HiddenAreaMeshRasterizerState.Get());
	
	// Set and draw the vertices
	UINT stride = sizeof(vr::HmdVector2_t);
	UINT offset = 0;
	m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_pContext->IASetInputLayout(m_HiddenAreaMeshInputLayout.Get());
	m_pContext->IASetVertexBuffers(0, 1, m_HiddenAreaMeshVertexBuffer.GetAddressOf(), &stride, &offset);
	m_pContext->Draw(m_HiddenAreaMeshNumVertices, 0);

	m_pContext->RSSetState(currentState);
}

void CompositorD3D::RenderTextureSwapChain(vr::EVREye eye, TextureBase* src, TextureBase* dst, ovrRecti viewport, vr::VRTextureBounds_t bounds, vr::HmdVector4_t quad)
{
	// TODO: Support compositing layers in DX12
	if (!m_pDevice)
		return;

	// Get the current state objects
	Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
	float blend_factor[4];
	uint32_t sample_mask;
	m_pContext->OMGetBlendState(blend_state.GetAddressOf(), blend_factor, &sample_mask);

	D3D11_PRIMITIVE_TOPOLOGY topology;
	m_pContext->IAGetPrimitiveTopology(&topology);

	ID3D11RasterizerState* ras_state;
	m_pContext->RSGetState(&ras_state);

	// Set the compositor shaders
	m_pContext->VSSetShader(m_VertexShader.Get(), NULL, 0);
	m_pContext->PSSetShader(m_CompositorShader.Get(), NULL, 0);
	ID3D11ShaderResourceView* resource = ((TextureD3D*)src)->Resource();
	m_pContext->PSSetShaderResources(0, 1, &resource);

	// Update the vertex buffer
	Vertex vertices[4] = {
		{ { quad.v[0], quad.v[2] },{ bounds.uMin, bounds.vMin } },
		{ { quad.v[1], quad.v[2] },{ bounds.uMax, bounds.vMin } },
		{ { quad.v[0], quad.v[3] },{ bounds.uMin, bounds.vMax } },
		{ { quad.v[1], quad.v[3] },{ bounds.uMax, bounds.vMax } }
	};
	D3D11_MAPPED_SUBRESOURCE map = { 0 };
	m_pContext->Map(m_VertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	memcpy(map.pData, vertices, sizeof(Vertex) * 4);
	m_pContext->Unmap(m_VertexBuffer.Get(), 0);

	// Prepare the render target
	D3D11_VIEWPORT vp = { (float)viewport.Pos.x, (float)viewport.Pos.y, (float)viewport.Size.w, (float)viewport.Size.h, D3D11_MIN_DEPTH, D3D11_MIN_DEPTH };
	m_pContext->RSSetViewports(1, &vp);
	ID3D11RenderTargetView* target = ((TextureD3D*)dst)->Target();
	m_pContext->OMSetRenderTargets(1, &target, nullptr);
	m_pContext->OMSetBlendState(m_BlendState.Get(), nullptr, -1);
	m_pContext->RSSetState(nullptr);

	// Set and draw the vertices
	uint32_t stride = sizeof(Vertex);
	uint32_t offset = 0;
	m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_pContext->IASetInputLayout(m_InputLayout.Get());
	m_pContext->IASetVertexBuffers(0, 1, m_VertexBuffer.GetAddressOf(), &stride, &offset);
	m_pContext->Draw(4, 0);

	// Restore the state objects
	m_pContext->RSSetState(ras_state);
	m_pContext->OMSetBlendState(blend_state.Get(), blend_factor, sample_mask);
	m_pContext->IASetPrimitiveTopology(topology);
}
