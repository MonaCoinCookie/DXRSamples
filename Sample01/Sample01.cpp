// Sample01.cpp: アプリケーションのエントリ ポイントを定義します。
//

#include "stdafx.h"
#include "Sample01.h"
#include <algorithm>
#include <vector>
#include <dxgi1_6.h>
#include "d3d12_1.h"
#include <atlbase.h>
#include "D3D12RaytracingFallback.h"
#include "CompiledShaders\test.r.h"
#include <sstream>
#include <iomanip>
#include <list>
#include <string>


namespace
{
	static const wchar_t* kWindowTitle = L"D3D12Sample";
	static const int kWindowWidth = 1280;
	static const int kWindowHeight = 720;
	static const int kMaxBuffers = 3;

	static LPCWSTR kRayGenName		= L"RayGenerator";
	static LPCWSTR kClosestHitName	= L"ClosestHitProcessor";
	static LPCWSTR kMissName		= L"MissProcessor";
	static LPCWSTR kHitGroupName	= L"HitGroup";

	struct Viewport
	{
		float left;
		float top;
		float right;
		float bottom;
	};

	struct RayGenCB
	{
		Viewport viewport;
		Viewport stencil;
	};

	template <typename T>
	class ObjPtr
	{
	public:
		ObjPtr()
		{}
		ObjPtr(T* p)
		{
			ptr_ = p;
		}
		~ObjPtr()
		{
			Destroy();
		}

		void Destroy()
		{
			if (ptr_ != nullptr)
			{
				ptr_->Release();
				ptr_ = nullptr;
			}
		}

		T*& Get()
		{
			return ptr_;
		}

		T* operator->()
		{
			return ptr_;
		}

	private:
		T*		ptr_ = nullptr;
	};	// class ComPtr

	struct Descriptor
	{
		D3D12_CPU_DESCRIPTOR_HANDLE	cpu_handle{};
		D3D12_GPU_DESCRIPTOR_HANDLE	gpu_handle{};
		UINT						index = 0;

		void Increment(UINT size)
		{
			cpu_handle.ptr += size;
			gpu_handle.ptr += size;
			index++;
		}
	};	// struct Descriptor

	HWND	g_hWnd_;

	ObjPtr<IDXGIFactory5>							g_pFactory_;
	ObjPtr<IDXGIAdapter3>							g_pAdapter_;
	ObjPtr<IDXGIOutput4>							g_pOutput_;
	ObjPtr<ID3D12Device>							g_pDevice_;
	ObjPtr<ID3D12CommandQueue>						g_pGraphicsQueue_;
	ObjPtr<ID3D12DescriptorHeap>					g_pDescHeaps_[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
	UINT											g_descSize_[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
	Descriptor										g_unusedHeapPtr_[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
	ObjPtr<IDXGISwapChain3>							g_pSwapchain_;
	ObjPtr<ID3D12Resource>							g_pSwapchainTex_[kMaxBuffers];
	Descriptor										g_swapchainRtv_[kMaxBuffers];
	ObjPtr<ID3D12Fence>								g_pPresentFence_;
	UINT											g_fenceValue_ = 0;
	HANDLE											g_fenceEvent_ = nullptr;
	ObjPtr<ID3D12CommandAllocator>					g_pCmdAllocator_;
	ObjPtr<ID3D12GraphicsCommandList>				g_pCmdLists_[kMaxBuffers];

	bool											g_isFallbackLayer = false;
	ObjPtr<ID3D12RaytracingFallbackDevice>			g_pFallbackDevice_;
	ObjPtr<ID3D12RaytracingFallbackCommandList>		g_pFallbackCmdLists_[kMaxBuffers];
	ObjPtr<ID3D12DeviceRaytracingPrototype>			g_pDxrDevice_;
	ObjPtr<ID3D12CommandListRaytracingPrototype>	g_pDxrCmdLists_[kMaxBuffers];
	ObjPtr<ID3D12RootSignature>						g_pGlobalRootSig_;
	ObjPtr<ID3D12RootSignature>						g_pLocalRootSig_;
	ObjPtr<ID3D12RaytracingFallbackStateObject>		g_pFallbackPSO_;
	ObjPtr<ID3D12StateObjectPrototype>				g_pDxrPSO_;
	ObjPtr<ID3D12Resource>							g_pResultOutput_;
	Descriptor										g_resultOutputDesc_;
	ObjPtr<ID3D12Resource>							g_pVB_, g_pIB_;
	ObjPtr<ID3D12Resource>							g_pTopAS_, g_pBottomAS_;
	WRAPPED_GPU_POINTER								g_topASPtr_;
	ObjPtr<ID3D12Resource>							g_pRayGenShaderTable;
	ObjPtr<ID3D12Resource>							g_pMissShaderTable;
	ObjPtr<ID3D12Resource>							g_pHitGroupShaderTable;

	int g_frameIndex_ = 0;

	// 指定個数の実験的フィーチャーを有効にする
	template <std::size_t N>
	inline bool EnableD3D12ExperimentalFeatures(UUID(&experimentalFeatures)[N])
	{
		ObjPtr<ID3D12Device> testDevice;
		return SUCCEEDED(D3D12EnableExperimentalFeatures(N, experimentalFeatures, nullptr, nullptr))
			&& SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice.Get())));
	}

	// Compute Shaderを利用したFallbackLayerを使用する場合に最初に呼び出す
	inline bool EnableComputeRaytracingFallback()
	{
		UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };
		return EnableD3D12ExperimentalFeatures(experimentalFeatures);
	}

	// DXRを使用する場合に最初に呼び出す
	inline bool EnableRaytracing()
	{
		UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels, D3D12RaytracingPrototype };
		return EnableD3D12ExperimentalFeatures(experimentalFeatures);
	}
}

// Window Proc
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// Handle destroy/shutdown messages.
	switch (message)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	// Handle any messages the switch statement didn't.
	return DefWindowProc(hWnd, message, wParam, lParam);
}

// Windowの初期化
void InitWindow(HINSTANCE hInstance, int nCmdShow)
{
	// Initialize the window class.
	WNDCLASSEX windowClass = { 0 };
	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = hInstance;
	windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	windowClass.lpszClassName = L"WindowClass1";
	RegisterClassEx(&windowClass);

	RECT windowRect = { 0, 0, kWindowWidth, kWindowHeight };
	AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	// Create the window and store a handle to it.
	g_hWnd_ = CreateWindowEx(NULL,
		L"WindowClass1",
		kWindowTitle,
		WS_OVERLAPPEDWINDOW,
		300,
		300,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		NULL,		// We have no parent window, NULL.
		NULL,		// We aren't using menus, NULL.
		hInstance,
		NULL);		// We aren't using multiple windows, NULL.

	ShowWindow(g_hWnd_, nCmdShow);
}

// D3D12のデバイスを生成する
bool InitDevice()
{
	g_isFallbackLayer = false;
	if (!EnableRaytracing())
	{
		if (!EnableComputeRaytracingFallback())
		{
			return false;
		}
		g_isFallbackLayer = true;
	}

	uint32_t factoryFlags = 0;
#ifdef _DEBUG
	// デバッグレイヤーの有効化
	{
		ID3D12Debug* debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			debugController->Release();
		}
	}

	// ファクトリをデバッグモードで作成する
	//factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	// ファクトリの生成
	auto hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&g_pFactory_.Get()));
	if (FAILED(hr))
	{
		return false;
	}

	// アダプタを取得する
	bool isWarp = false;
	ObjPtr<IDXGIAdapter1> pAdapter;
	hr = g_pFactory_->EnumAdapters1(0, &pAdapter.Get());
	if (FAILED(hr))
	{
		// 取得できない場合はWarpアダプタを取得
		pAdapter.Destroy();

		hr = g_pFactory_->EnumWarpAdapter(IID_PPV_ARGS(&pAdapter.Get()));
		if (FAILED(hr))
		{
			return false;
		}
		isWarp = true;
	}
	hr = pAdapter->QueryInterface(IID_PPV_ARGS(&g_pAdapter_.Get()));
	if (FAILED(hr))
	{
		return false;
	}

	if (!isWarp)
	{
		// ディスプレイを取得する
		ObjPtr<IDXGIOutput> pOutput;
		hr = g_pAdapter_->EnumOutputs(0, &pOutput.Get());
		if (FAILED(hr))
		{
			return false;
		}

		hr = pOutput->QueryInterface(IID_PPV_ARGS(&g_pOutput_.Get()));
		if (FAILED(hr))
		{
			return false;
		}
	}

	// デバイスの生成
	hr = D3D12CreateDevice(g_pAdapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_pDevice_.Get()));
	if (FAILED(hr))
	{
		return false;
	}

	// Queueの作成
	{
		D3D12_COMMAND_QUEUE_DESC desc{};
		desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;		// GPUタイムアウトが有効
		desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		auto hr = g_pDevice_->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_pGraphicsQueue_.Get()));
		if (FAILED(hr))
		{
			return false;
		}
	}

	// DescriptorHeapの作成
	for (int i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; i++)
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc{};

		desc.Type = (D3D12_DESCRIPTOR_HEAP_TYPE)i;
		desc.NumDescriptors = 100;
		if (i == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || i == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
		{
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		}
		else
		{
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		}

		auto hr = g_pDevice_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pDescHeaps_[i].Get()));
		if (FAILED(hr))
		{
			return false;
		}

		g_descSize_[i] = g_pDevice_->GetDescriptorHandleIncrementSize(desc.Type);
		g_unusedHeapPtr_[i].cpu_handle = g_pDescHeaps_[i]->GetCPUDescriptorHandleForHeapStart();
		g_unusedHeapPtr_[i].gpu_handle = g_pDescHeaps_[i]->GetGPUDescriptorHandleForHeapStart();
	}

	// Swapchainの作成
	{
		DXGI_SWAP_CHAIN_DESC desc = {};
		desc.BufferCount = kMaxBuffers;
		desc.BufferDesc.Width = kWindowWidth;
		desc.BufferDesc.Height = kWindowHeight;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.OutputWindow = g_hWnd_;
		desc.SampleDesc.Count = 1;
		desc.Windowed = true;

		ObjPtr<IDXGISwapChain> pSwap;
		auto hr = g_pFactory_->CreateSwapChain(g_pGraphicsQueue_.Get(), &desc, &pSwap.Get());
		if (FAILED(hr))
		{
			return false;
		}

		hr = pSwap->QueryInterface(IID_PPV_ARGS(&g_pSwapchain_.Get()));
		if (FAILED(hr))
		{
			return false;
		}

		g_frameIndex_ = g_pSwapchain_->GetCurrentBackBufferIndex();

		for (int i = 0; i < kMaxBuffers; i++)
		{
			g_pSwapchain_->GetBuffer(i, IID_PPV_ARGS(&g_pSwapchainTex_[i].Get()));

			D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
			viewDesc.Format = g_pSwapchainTex_[i]->GetDesc().Format;
			viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			viewDesc.Texture2D.MipSlice = 0;
			viewDesc.Texture2D.PlaneSlice = 0;

			g_swapchainRtv_[i].cpu_handle = g_unusedHeapPtr_[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].cpu_handle;
			g_swapchainRtv_[i].gpu_handle = g_unusedHeapPtr_[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].gpu_handle;
			g_unusedHeapPtr_[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].Increment(g_descSize_[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]);

			g_pDevice_->CreateRenderTargetView(g_pSwapchainTex_[i].Get(), &viewDesc, g_swapchainRtv_[i].cpu_handle);
		}
	}

	// Fenceの作成
	hr = g_pDevice_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pPresentFence_.Get()));
	if (FAILED(hr))
	{
		return false;
	}
	g_fenceValue_ = 1;

	g_fenceEvent_ = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
	if (g_fenceEvent_ == nullptr)
	{
		return false;
	}

	// コマンドリストの作成
	hr = g_pDevice_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_pCmdAllocator_.Get()));
	if (FAILED(hr))
	{
		return false;
	}

	for (auto&& v : g_pCmdLists_)
	{
		hr = g_pDevice_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_pCmdAllocator_.Get(), nullptr, IID_PPV_ARGS(&v.Get()));
		if (FAILED(hr))
		{
			return false;
		}

		v->Close();
	}

	return true;
}

void DestroyDevice()
{
	for (auto&& v : g_pCmdLists_) v.Destroy();
	g_pCmdAllocator_.Destroy();

	g_pPresentFence_.Destroy();

	for (auto&& v : g_pSwapchainTex_) v.Destroy();
	g_pSwapchain_.Destroy();

	for (auto&& v : g_pDescHeaps_) v.Destroy();
	g_pGraphicsQueue_.Destroy();
	g_pDevice_.Destroy();

	g_pOutput_.Destroy();
	g_pAdapter_.Destroy();
	g_pFactory_.Destroy();
}

void WaitDrawDone()
{
	// 現在のFence値がコマンド終了後にFenceに書き込まれるようにする
	UINT64 fvalue = g_fenceValue_;
	g_pGraphicsQueue_->Signal(g_pPresentFence_.Get(), fvalue);
	g_fenceValue_++;

	// まだコマンドキューが終了していないことを確認する
	// ここまででコマンドキューが終了してしまうとイベントが一切発火されなくなるのでチェックしている
	if (g_pPresentFence_->GetCompletedValue() < fvalue)
	{
		// このFenceにおいて、fvalue の値になったらイベントを発火させる
		g_pPresentFence_->SetEventOnCompletion(fvalue, g_fenceEvent_);
		// イベントが発火するまで待つ
		WaitForSingleObject(g_fenceEvent_, INFINITE);
	}
}

bool InitRaytraceDevice()
{
	if (g_isFallbackLayer)
	{
		auto hr = D3D12CreateRaytracingFallbackDevice(g_pDevice_.Get(), CreateRaytracingFallbackDeviceFlags::None, 0, IID_PPV_ARGS(&g_pFallbackDevice_.Get()));
		if (FAILED(hr))
		{
			return false;
		}
		for (int i = 0; i < kMaxBuffers; i++)
			g_pFallbackDevice_->QueryRaytracingCommandList(g_pCmdLists_[i].Get(), IID_PPV_ARGS(&g_pFallbackCmdLists_[i].Get()));
	}
	else // DirectX Raytracing
	{
		auto hr = g_pDevice_->QueryInterface(IID_PPV_ARGS(&g_pDxrDevice_.Get()));
		if (FAILED(hr))
		{
			return false;
		}

		for (int i = 0; i < kMaxBuffers; i++)
		{
			hr = g_pCmdLists_[i]->QueryInterface(IID_PPV_ARGS(&g_pDxrCmdLists_[i].Get()));
			if (FAILED(hr))
			{
				return false;
			}
		}
	}

	return true;
}

void DestroyRaytraceDevice()
{
	for (auto&& v : g_pDxrCmdLists_) v.Destroy();
	g_pDxrDevice_.Destroy();
	for (auto&& v : g_pFallbackCmdLists_) v.Destroy();
	g_pFallbackDevice_.Destroy();
}

bool CreateRootSig(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3D12RootSignature** ppSig)
{
	ObjPtr<ID3DBlob> blob;
	ObjPtr<ID3DBlob> error;

	if (g_isFallbackLayer)
	{
		auto hr = g_pFallbackDevice_->D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob.Get(), &error.Get());
		if (FAILED(hr))
		{
			return false;
		}

		hr = g_pFallbackDevice_->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(ppSig));
		if (FAILED(hr))
		{
			return false;
		}
	}
	else // DirectX Raytracing
	{
		auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob.Get(), &error.Get());
		if (FAILED(hr))
		{
			return false;
		}

		hr = g_pDevice_->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(ppSig));
		if (FAILED(hr))
		{
			return false;
		}
	}
	return true;
}

inline void PrintStateObjectDesc(const D3D12_STATE_OBJECT_DESC* desc)
{
	std::wstringstream wstr;
	wstr << L"D3D12 State Object 0x" << static_cast<const void*>(desc) << L": ";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) wstr << L"Collection\n";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) wstr << L"Raytracing Pipeline\n";

	auto ExportTree = [](UINT depth, UINT numExports, const D3D12_EXPORT_DESC* exports)
	{
		std::wostringstream woss;
		for (UINT i = 0; i < numExports; i++)
		{
			if (depth > 0)
			{
				for (UINT j = 0; j < 2 * depth - 1; j++) woss << L" ";
				woss << (i == numExports - 1 ? L"\xC0" : L"\xC3");
			}
			woss << L"[" << i << L"]: ";
			if (exports[i].ExportToRename) woss << exports[i].ExportToRename << L" --> ";
			woss << exports[i].Name << L"\n";
		}
		return woss.str();
	};

	for (UINT i = 0; i < desc->NumSubobjects; i++)
	{
		wstr << L"[" << i << L"]: ";
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_FLAGS)
		{
			wstr << L"Flags (not yet defined)\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE)
		{
			wstr << L"Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE)
		{
			wstr << L"Local Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK)
		{
			wstr << L"Node Mask: 0x" << std::hex << std::setfill(L'0') << std::setw(8) << *static_cast<const UINT*>(desc->pSubobjects[i].pDesc) << std::setw(0) << std::dec << L"\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_CACHED_STATE_OBJECT)
		{
			wstr << L"Cached State Object (not yet defined)\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY)
		{
			wstr << L"DXIL Library 0x";
			auto lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(desc->pSubobjects[i].pDesc);
			wstr << lib->DXILLibrary.pShaderBytecode << L", " << lib->DXILLibrary.BytecodeLength << L" bytes\n";
			wstr << ExportTree(1, lib->NumExports, lib->pExports);
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION)
		{
			wstr << L"Existing Library 0x";
			auto collection = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(desc->pSubobjects[i].pDesc);
			wstr << collection->pExistingCollection << L"\n";
			wstr << ExportTree(1, collection->NumExports, collection->pExports);
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION)
		{
			wstr << L"Subobject to Exports Association (Subobject [";
			auto association = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
			UINT index = static_cast<UINT>(association->pSubobjectToAssociate - desc->pSubobjects);
			wstr << index << L"])\n";
			for (UINT j = 0; j < association->NumExports; j++) wstr << (j == association->NumExports - 1 ? L" \xC0" : L" \xC3") << L"[" << j << L"]: " << association->pExports[j] << L"\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION)
		{
			wstr << L"DXIL Subobjects to Exports Association (";
			auto association = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
			wstr << association->SubobjectToAssociate << L")\n";
			for (UINT j = 0; j < association->NumExports; j++) wstr << (j == association->NumExports - 1 ? L" \xC0" : L" \xC3") << L"[" << j << L"]: " << association->pExports[j] << L"\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG)
		{
			wstr << L"Raytracing Shader Config\n";
			auto config = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(desc->pSubobjects[i].pDesc);
			wstr << L" \xC3" << L"[0]: Max Payload Size: " << config->MaxPayloadSizeInBytes << L" bytes\n";
			wstr << L" \xC0" << L"[1]: Max Attribute Size: " << config->MaxAttributeSizeInBytes << L" bytes\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG)
		{
			wstr << L"Raytracing Pipeline Config\n";
			auto config = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(desc->pSubobjects[i].pDesc);
			wstr << L" \xC0" << L"[0]: Max Recursion Depth: " << config->MaxTraceRecursionDepth << L"\n";
		}
		if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP)
		{
			wstr << L"Hit Group (";
			auto hitGroup = static_cast<const D3D12_HIT_GROUP_DESC*>(desc->pSubobjects[i].pDesc);
			wstr << (hitGroup->HitGroupExport ? hitGroup->HitGroupExport : L"[none]") << L")\n";
			wstr << L" \xC3" << L"[0]: Any Hit Import: " << (hitGroup->AnyHitShaderImport ? hitGroup->AnyHitShaderImport : L"[none]") << L"\n";
			wstr << L" \xC3" << L"[1]: Closest Hit Import: " << (hitGroup->ClosestHitShaderImport ? hitGroup->ClosestHitShaderImport : L"[none]") << L"\n";
			wstr << L" \xC0" << L"[2]: Intersection Import: " << (hitGroup->IntersectionShaderImport ? hitGroup->IntersectionShaderImport : L"[none]") << L"\n";
		}
	}
	OutputDebugStringW(wstr.str().c_str());
}

bool InitRaytracePipeline()
{
	// ルートシグネチャを作成する
	// ルートシグネチャはRayTracer全体で使用するグローバルと、各ヒットシェーダで使用するローカルの2種類が必要っぽい
	{
		D3D12_DESCRIPTOR_RANGE ranges[] = {
			{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
			{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
		};
		D3D12_ROOT_PARAMETER params[ARRAYSIZE(ranges)];
		for (int i = 0; i < ARRAYSIZE(ranges); i++)
		{
			params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[i].DescriptorTable.NumDescriptorRanges = 1;
			params[i].DescriptorTable.pDescriptorRanges = ranges + i;
		}
		D3D12_ROOT_SIGNATURE_DESC sigDesc{};
		sigDesc.NumParameters = ARRAYSIZE(params);
		sigDesc.pParameters = params;
		sigDesc.NumStaticSamplers = 0;
		sigDesc.pStaticSamplers = nullptr;
		sigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		if (!CreateRootSig(sigDesc, &g_pGlobalRootSig_.Get()))
		{
			return false;
		}
	}
	{
		D3D12_ROOT_PARAMETER params[1];
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.RegisterSpace = 0;
		params[0].Constants.Num32BitValues = sizeof(RayGenCB);
		D3D12_ROOT_SIGNATURE_DESC sigDesc{};
		sigDesc.NumParameters = ARRAYSIZE(params);
		sigDesc.pParameters = params;
		sigDesc.NumStaticSamplers = 0;
		sigDesc.pStaticSamplers = nullptr;
		sigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		if (!CreateRootSig(sigDesc, &g_pLocalRootSig_.Get()))
		{
			return false;
		}
	}

	std::vector<D3D12_STATE_SUBOBJECT> subobjects;
	subobjects.reserve(32);
	auto AddSubobject = [&](D3D12_STATE_SUBOBJECT_TYPE type, const void* desc)
	{
		D3D12_STATE_SUBOBJECT sub;
		sub.Type = type;
		sub.pDesc = desc;
		subobjects.push_back(sub);
	};

	// DXILライブラリサブオブジェクト
	// ライブラリとしてコンパイルされたシェーダには複数のシェーダプログラムが存在してるっぽい
	// ここから必要なシェーダをエクスポートする
	D3D12_EXPORT_DESC libExport[] = {
		{ kRayGenName,		nullptr, D3D12_EXPORT_FLAG_NONE },
		{ kClosestHitName,	nullptr, D3D12_EXPORT_FLAG_NONE },
		{ kMissName,		nullptr, D3D12_EXPORT_FLAG_NONE },
	};

	D3D12_DXIL_LIBRARY_DESC dxilDesc{};
	dxilDesc.DXILLibrary.pShaderBytecode = g_pTestShader;
	dxilDesc.DXILLibrary.BytecodeLength = sizeof(g_pTestShader);
	dxilDesc.NumExports = ARRAYSIZE(libExport);
	dxilDesc.pExports = libExport;
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxilDesc);

	// ヒットグループサブオブジェクト
	// ヒットグループはレイがヒットした場合の処理のグループ
	// 基本はマテリアルの数？ もしくはマテリアルで使用されるヒットシェーダの数？
	D3D12_HIT_GROUP_DESC hitGroupDesc{};
	hitGroupDesc.HitGroupExport = kHitGroupName;
	hitGroupDesc.ClosestHitShaderImport = kClosestHitName;
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroupDesc);

	// シェーダコンフィグサブオブジェクト
	// ヒットシェーダ、ミスシェーダの引数となるPayload, IntersectionAttributesの最大サイズを設定する
	D3D12_RAYTRACING_SHADER_CONFIG shaderConfigDesc{};
	shaderConfigDesc.MaxPayloadSizeInBytes = sizeof(float) * 4;		// float4 color
	shaderConfigDesc.MaxAttributeSizeInBytes = sizeof(float) * 2;	// float2 barycentrics
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfigDesc);

	// ローカルルートシグネチャサブオブジェクト
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &g_pLocalRootSig_.Get());

	// Exports Assosiation サブオブジェクト
	// ルートシグネチャとシェーダテーブルのバインダー的なもの？
	LPCWSTR kExports[] = {
		kRayGenName,
		kMissName,
		kHitGroupName,
	};
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assocDesc{};
	assocDesc.pSubobjectToAssociate = &subobjects.back();
	assocDesc.NumExports = ARRAYSIZE(kExports);
	assocDesc.pExports = kExports;
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &assocDesc);

	// グローバルルートシグネチャサブオブジェクト
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &g_pGlobalRootSig_.Get());

	// レイトレースコンフィグサブオブジェクト
	// シェーダ内でTraceRay()を行うことができる最大深度
	// 1以上を設定する必要があると思われる
	D3D12_RAYTRACING_PIPELINE_CONFIG rtConfigDesc{};
	rtConfigDesc.MaxTraceRecursionDepth = 1;
	AddSubobject(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &rtConfigDesc);

	// PSO生成
	D3D12_STATE_OBJECT_DESC psoDesc{};
	psoDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	psoDesc.pSubobjects = subobjects.data();
	psoDesc.NumSubobjects = (UINT)subobjects.size();

	PrintStateObjectDesc(&psoDesc);

	if (g_isFallbackLayer)
	{
		auto hr = g_pFallbackDevice_->CreateStateObject(&psoDesc, IID_PPV_ARGS(&g_pFallbackPSO_.Get()));
		if (FAILED(hr))
		{
			return false;
		}
	}
	else // DirectX Raytracing
	{
		auto hr = g_pDxrDevice_->CreateStateObject(&psoDesc, IID_PPV_ARGS(&g_pDxrPSO_.Get()));
		if (FAILED(hr))
		{
			return false;
		}
	}

	// 出力先UAVを生成
	{
		auto backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

		D3D12_RESOURCE_DESC uavDesc{};
		uavDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		uavDesc.Alignment = 0;
		uavDesc.Width = kWindowWidth;
		uavDesc.Height = kWindowHeight;
		uavDesc.DepthOrArraySize = 1;
		uavDesc.MipLevels = 1;
		uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uavDesc.SampleDesc.Count = 1;
		uavDesc.SampleDesc.Quality = 0;
		uavDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		D3D12_HEAP_PROPERTIES heapProp{};
		heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heapProp.CreationNodeMask = 1;
		heapProp.VisibleNodeMask = 1;

		auto hr = g_pDevice_->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&uavDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr,
			IID_PPV_ARGS(&g_pResultOutput_.Get()));
		if (FAILED(hr))
		{
			return false;
		}

		g_resultOutputDesc_ = g_unusedHeapPtr_[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
		g_unusedHeapPtr_->Increment(g_descSize_[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]);

		D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
		viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		g_pDevice_->CreateUnorderedAccessView(g_pResultOutput_.Get(), nullptr, &viewDesc, g_resultOutputDesc_.cpu_handle);
	}

	return true;
}

void DestroyRaytracePipeline()
{
	g_pFallbackPSO_.Destroy();
	g_pDxrPSO_.Destroy();

	g_pGlobalRootSig_.Destroy();
	g_pLocalRootSig_.Destroy();
}

bool CreateUploadBuffer(void* pData, size_t dataSize, ID3D12Resource** ppRes)
{
	D3D12_HEAP_PROPERTIES heapProp{};
	heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = dataSize;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	auto hr = g_pDevice_->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(ppRes));
	if (FAILED(hr))
	{
		return false;
	}

	void *pMappedData;
	(*ppRes)->Map(0, nullptr, &pMappedData);
	memcpy(pMappedData, pData, dataSize);
	(*ppRes)->Unmap(0, nullptr);

	return true;
}

bool InitGeometry()
{
	const float size = 0.7f;
	const float depth = 1.0f;
	float vertices[] = {
		 size, -size, depth,
		-size, -size, depth,
		 size,  size, depth,
		-size,  size, depth,
	};
	UINT16 indices[] =
	{
		0, 1, 2,
		1, 3, 2,
	};

	if (!CreateUploadBuffer(vertices, sizeof(vertices), &g_pVB_.Get()))
	{
		return false;
	}

	if (!CreateUploadBuffer(indices, sizeof(indices), &g_pIB_.Get()))
	{
		return false;
	}

	return true;
}

void DestroyGeometry()
{
	g_pIB_.Destroy();
	g_pVB_.Destroy();
}

bool CreateAccelerationStructure(UINT64 size, D3D12_RESOURCE_STATES initialState, ID3D12Resource** ppRes)
{
	D3D12_HEAP_PROPERTIES heapProp{};
	heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProp.CreationNodeMask = 1;
	heapProp.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	auto hr = g_pDevice_->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(ppRes));
	if (FAILED(hr))
		return true;

	return true;
}

WRAPPED_GPU_POINTER CreateFallbackWrappedPointer(ID3D12Resource* resource, UINT bufferNumElements)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
	desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	desc.Format = DXGI_FORMAT_R32_TYPELESS;
	desc.Buffer.NumElements = bufferNumElements;

	D3D12_CPU_DESCRIPTOR_HANDLE bottomDesc{};

	Descriptor uavDesc;
	if (!g_pFallbackDevice_->UsingRaytracingDriver())
	{
		uavDesc = g_unusedHeapPtr_[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
		g_unusedHeapPtr_->Increment(g_descSize_[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]);

		g_pDevice_->CreateUnorderedAccessView(resource, nullptr, &desc, uavDesc.cpu_handle);
	}
	return g_pFallbackDevice_->GetWrappedPointerSimple(uavDesc.index, resource->GetGPUVirtualAddress());
}

bool InitAccelerationStructure()
{
	auto&& cmdList = g_pCmdLists_[0];
	auto&& fallbackCmdList = g_pFallbackCmdLists_[0];

	// Acceleration Structureの生成はコマンドリストに積まれて処理される
	// GPUで処理してる？
	cmdList->Reset(g_pCmdAllocator_.Get(), nullptr);

	// ジオメトリ記述子
	// ジオメトリ1つの頂点バッファ、インデックスバッファを設定
	// ジオメトリタイプは複数選べるが、トライアングルにしておけば普通のポリゴンモデルが使用できる
	D3D12_RAYTRACING_GEOMETRY_DESC geoDesc{};
	geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geoDesc.Triangles.IndexBuffer = g_pIB_->GetGPUVirtualAddress();
	geoDesc.Triangles.IndexCount = static_cast<UINT>(g_pIB_->GetDesc().Width) / sizeof(UINT16);
	geoDesc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
	geoDesc.Triangles.Transform = 0;
	geoDesc.Triangles.VertexBuffer.StartAddress = g_pVB_->GetGPUVirtualAddress();
	geoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
	geoDesc.Triangles.VertexCount = static_cast<UINT>(g_pVB_->GetDesc().Width) / geoDesc.Triangles.VertexBuffer.StrideInBytes;
	geoDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

	auto buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;	// トレースを高速にするための構築を行う？

	// 各レベルのASに必要なバッファサイズを取得する
	// ASにはボトムレベルとトップレベルがあり、両方ともASを生成する必要がある
	// ボトムレベルはトライアングルスープによって構築される、ジオメトリ1つを定義するAS
	// トップレベルはボトムレベルのインスタンスなので、トライアングル情報は持たず、参照するボトムレベルとトランスフォーム情報を持つ
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topPrebuildInfo{};
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomPrebuildInfo{};
	{
		D3D12_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_DESC desc{};
		desc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		desc.Flags = buildFlags;
		desc.NumDescs = 1;
		desc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		desc.pGeometryDescs = nullptr;
		if (g_isFallbackLayer)
		{
			g_pFallbackDevice_->GetRaytracingAccelerationStructurePrebuildInfo(&desc, &topPrebuildInfo);
		}
		else // DirectX Raytracing
		{
			g_pFallbackDevice_->GetRaytracingAccelerationStructurePrebuildInfo(&desc, &topPrebuildInfo);
		}
		if (topPrebuildInfo.ResultDataMaxSizeInBytes == 0)
			return false;

		desc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		desc.pGeometryDescs = &geoDesc;
		if (g_isFallbackLayer)
		{
			g_pFallbackDevice_->GetRaytracingAccelerationStructurePrebuildInfo(&desc, &bottomPrebuildInfo);
		}
		else // DirectX Raytracing
		{
			g_pDxrDevice_->GetRaytracingAccelerationStructurePrebuildInfo(&desc, &bottomPrebuildInfo);
		}
		if (bottomPrebuildInfo.ResultDataMaxSizeInBytes == 0)
			return false;
	}

	// スクラッチリソースを作成する
	// スクラッチリソースはAS構築時に使用する一時バッファ
	// ASを生成してしまえば基本不要
	ObjPtr<ID3D12Resource> pScrachResource;
	if (!CreateAccelerationStructure(
		std::max<UINT64>(topPrebuildInfo.ScratchDataSizeInBytes, bottomPrebuildInfo.ScratchDataSizeInBytes),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		&pScrachResource.Get()))
	{
		return false;
	}

	// トップとボトムのASを生成する
	{
		D3D12_RESOURCE_STATES initialState;
		if (g_isFallbackLayer)
		{
			initialState = g_pFallbackDevice_->GetAccelerationStructureResourceState();
		}
		else // DirectX Raytracing
		{
			initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		}

		if (!CreateAccelerationStructure(topPrebuildInfo.ResultDataMaxSizeInBytes, initialState, &g_pTopAS_.Get()))
			return false;
		if (!CreateAccelerationStructure(bottomPrebuildInfo.ResultDataMaxSizeInBytes, initialState, &g_pBottomAS_.Get()))
			return false;
	}

	// トップレベルに登録するインスタンスのバッファを構築する
	ObjPtr<ID3D12Resource> pInstances;
	if (g_isFallbackLayer)
	{
		// トップレベルに登録するインスタンス記述子
		D3D12_RAYTRACING_FALLBACK_INSTANCE_DESC desc{};
		desc.Transform[0] = desc.Transform[5] = desc.Transform[10] = 1;		// トランスフォーム行列は単位行列
		desc.InstanceMask = 1;
		UINT numBufferElements = static_cast<UINT>(bottomPrebuildInfo.ResultDataMaxSizeInBytes) / sizeof(UINT32);
		desc.AccelerationStructure = CreateFallbackWrappedPointer(g_pBottomAS_.Get(), numBufferElements);
		if (!CreateUploadBuffer(&desc, sizeof(desc), &pInstances.Get()))
		{
			return false;
		}

		// FallbackLayerの場合はトップレベルASのポインタを直接取得できないので、面倒だけどこの形で取得しておく
		numBufferElements = static_cast<UINT>(topPrebuildInfo.ResultDataMaxSizeInBytes) / sizeof(UINT32);
		g_topASPtr_ = CreateFallbackWrappedPointer(g_pTopAS_.Get(), numBufferElements);
	}
	else
	{
		D3D12_RAYTRACING_INSTANCE_DESC desc{};
		desc.Transform[0] = desc.Transform[5] = desc.Transform[10] = 1;
		desc.InstanceMask = 1;
		desc.AccelerationStructure = g_pBottomAS_->GetGPUVirtualAddress();
		if (!CreateUploadBuffer(&desc, sizeof(desc), &pInstances.Get()))
		{
			return false;
		}
	}

	// ボトムレベルASを構築するための記述子
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomBuildDesc{};
	{
		bottomBuildDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		bottomBuildDesc.Flags = buildFlags;
		bottomBuildDesc.ScratchAccelerationStructureData = { pScrachResource->GetGPUVirtualAddress(), pScrachResource->GetDesc().Width };
		bottomBuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		bottomBuildDesc.DestAccelerationStructureData = { g_pBottomAS_->GetGPUVirtualAddress(), bottomPrebuildInfo.ResultDataMaxSizeInBytes };
		bottomBuildDesc.NumDescs = 1;
		bottomBuildDesc.pGeometryDescs = &geoDesc;
	}

	// トップレベルASを構築するための記述子
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuildDesc = bottomBuildDesc;
	{
		topBuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		topBuildDesc.DestAccelerationStructureData = { g_pTopAS_->GetGPUVirtualAddress(), topPrebuildInfo.ResultDataMaxSizeInBytes };
		topBuildDesc.NumDescs = 1;
		topBuildDesc.pGeometryDescs = nullptr;
		topBuildDesc.InstanceDescs = pInstances->GetGPUVirtualAddress();
		topBuildDesc.ScratchAccelerationStructureData = { pScrachResource->GetGPUVirtualAddress(), pScrachResource->GetDesc().Width };
	}

	// AS構築ラムダ
	auto BuildAccelerationStructure = [&](auto* raytracingCommandList)
	{
		raytracingCommandList->BuildRaytracingAccelerationStructure(&bottomBuildDesc);

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.UAV.pResource = g_pBottomAS_.Get();
		cmdList->ResourceBarrier(1, &barrier);
		
		raytracingCommandList->BuildRaytracingAccelerationStructure(&topBuildDesc);
	};

	// ASを構築する
	if (g_isFallbackLayer)
	{
		// Fallbackレイヤーの場合はCSを利用するため、デスクリプタヒープを設定しておく必要がある
		ID3D12DescriptorHeap* pDescriptorHeaps[] = { g_pDescHeaps_[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Get() };
		fallbackCmdList->SetDescriptorHeaps(ARRAYSIZE(pDescriptorHeaps), pDescriptorHeaps);
		BuildAccelerationStructure(fallbackCmdList.Get());
	}
	else // DirectX Raytracing
	{
		BuildAccelerationStructure(g_pDxrCmdLists_[0].Get());
	}

	// コマンド実行
	cmdList->Close();
	ID3D12CommandList* cmdLists[] = { cmdList.Get() };
	g_pGraphicsQueue_->ExecuteCommandLists(ARRAYSIZE(cmdLists), cmdLists);

	// コマンド完了待ち
	WaitDrawDone();

	return true;
}

void DestroyAccelerationStructure()
{
	g_pTopAS_.Destroy();
	g_pBottomAS_.Destroy();
}

bool InitShaderTable()
{
	void* rayGenShaderIdentifier;
	void* missShaderIdentifier;
	void* hitGroupShaderIdentifier;

	// Shader Identifierを取得する
	UINT shaderIdentifierSize;
	if (g_isFallbackLayer)
	{
		rayGenShaderIdentifier		= g_pFallbackPSO_->GetShaderIdentifier(kRayGenName);
		missShaderIdentifier		= g_pFallbackPSO_->GetShaderIdentifier(kMissName);
		hitGroupShaderIdentifier	= g_pFallbackPSO_->GetShaderIdentifier(kHitGroupName);
		shaderIdentifierSize		= g_pFallbackDevice_->GetShaderIdentifierSize();
	}
	else // DirectX Raytracing
	{
		ObjPtr<ID3D12StateObjectPropertiesPrototype> prop;
		g_pDxrPSO_->QueryInterface(IID_PPV_ARGS(&prop.Get()));
		rayGenShaderIdentifier		= prop->GetShaderIdentifier(kRayGenName);
		missShaderIdentifier		= prop->GetShaderIdentifier(kMissName);
		hitGroupShaderIdentifier	= prop->GetShaderIdentifier(kHitGroupName);
		shaderIdentifierSize		= g_pDxrDevice_->GetShaderIdentifierSize();
	}

	// Initialize shader records.
	struct RootArguments {
		RayGenCB cb;
	} rootArguments;
	rootArguments.cb.viewport = { -1.0f, -1.0f, 1.0f, 1.0f };
	rootArguments.cb.stencil = { -0.9f, -0.9f, 0.9f, 0.9f };
	UINT rootArgumentsSize = sizeof(rootArguments);

	// Shader record = {{ Shader ID }, { RootArguments }}
	UINT shaderRecordSize = shaderIdentifierSize + rootArgumentsSize;

	auto GenShaderTable = [&](void* shaderId, size_t shaderIdSize, void* rootArg, size_t rootArgSize, ID3D12Resource** ppRes)
	{
		D3D12_HEAP_PROPERTIES heapProp{};
		heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heapProp.CreationNodeMask = 1;
		heapProp.VisibleNodeMask = 1;

		size_t allSize = shaderIdSize + rootArgSize;
		allSize = (allSize + D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1) / D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT * D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;

		D3D12_RESOURCE_DESC resDesc{};
		resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resDesc.Alignment = 0;
		resDesc.Width = allSize;
		resDesc.Height = 1;
		resDesc.DepthOrArraySize = 1;
		resDesc.MipLevels = 1;
		resDesc.Format = DXGI_FORMAT_UNKNOWN;
		resDesc.SampleDesc.Count = 1;
		resDesc.SampleDesc.Quality = 0;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		auto hr = g_pDevice_->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(ppRes));
		if (FAILED(hr))
		{
			return false;
		}

		uint8_t *pMappedData;
		(*ppRes)->Map(0, nullptr, reinterpret_cast<void**>(&pMappedData));
		memcpy(pMappedData, shaderId, shaderIdSize);
		memcpy(pMappedData + shaderIdSize, rootArg, rootArgSize);
		(*ppRes)->Unmap(0, nullptr);

		return true;
	};

	if (!GenShaderTable(rayGenShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(rootArguments), &g_pRayGenShaderTable.Get()))
	{
		return false;
	}
	if (!GenShaderTable(missShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(rootArguments), &g_pMissShaderTable.Get()))
	{
		return false;
	}
	if (!GenShaderTable(hitGroupShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(rootArguments), &g_pHitGroupShaderTable.Get()))
	{
		return false;
	}

	return true;
}

void DestroyShaderTable()
{
	g_pRayGenShaderTable.Destroy();
	g_pMissShaderTable.Destroy();
	g_pHitGroupShaderTable.Destroy();
}

void LetsRaytracing()
{
	auto&& cmdList = g_pCmdLists_[g_frameIndex_];

	// グローバルルートシグネチャを設定
	cmdList->SetComputeRootSignature(g_pGlobalRootSig_.Get());

	ID3D12DescriptorHeap* descHeaps[] = {
		g_pDescHeaps_[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Get(),
		g_pDescHeaps_[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER].Get(),
	};

	// Bind the heaps, acceleration structure and dispatch rays.    
	if (g_isFallbackLayer)
	{
		auto&& fallbackCmdList = g_pFallbackCmdLists_[g_frameIndex_];

		fallbackCmdList->SetDescriptorHeaps(ARRAYSIZE(descHeaps), descHeaps);
		cmdList->SetComputeRootDescriptorTable(0, g_resultOutputDesc_.gpu_handle);
		fallbackCmdList->SetTopLevelAccelerationStructure(1, g_topASPtr_);

		D3D12_FALLBACK_DISPATCH_RAYS_DESC desc{};
		desc.HitGroupTable.StartAddress = g_pHitGroupShaderTable->GetGPUVirtualAddress();
		desc.HitGroupTable.SizeInBytes = g_pHitGroupShaderTable->GetDesc().Width;
		desc.HitGroupTable.StrideInBytes = desc.HitGroupTable.SizeInBytes;
		desc.MissShaderTable.StartAddress = g_pMissShaderTable->GetGPUVirtualAddress();
		desc.MissShaderTable.SizeInBytes = g_pMissShaderTable->GetDesc().Width;
		desc.MissShaderTable.StrideInBytes = desc.MissShaderTable.SizeInBytes;
		desc.RayGenerationShaderRecord.StartAddress = g_pRayGenShaderTable->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = g_pRayGenShaderTable->GetDesc().Width;
		desc.Width = kWindowWidth;
		desc.Height = kWindowHeight;
		fallbackCmdList->DispatchRays(g_pFallbackPSO_.Get(), &desc);
	}
	else // DirectX Raytracing
	{
		auto&& dxrCmdList = g_pDxrCmdLists_[g_frameIndex_];

		cmdList->SetDescriptorHeaps(ARRAYSIZE(descHeaps), descHeaps);
		cmdList->SetComputeRootDescriptorTable(0, g_resultOutputDesc_.gpu_handle);
		cmdList->SetComputeRootShaderResourceView(1, g_pTopAS_->GetGPUVirtualAddress());

		D3D12_DISPATCH_RAYS_DESC desc{};
		desc.HitGroupTable.StartAddress = g_pHitGroupShaderTable->GetGPUVirtualAddress();
		desc.HitGroupTable.SizeInBytes = g_pHitGroupShaderTable->GetDesc().Width;
		desc.HitGroupTable.StrideInBytes = desc.HitGroupTable.SizeInBytes;
		desc.MissShaderTable.StartAddress = g_pMissShaderTable->GetGPUVirtualAddress();
		desc.MissShaderTable.SizeInBytes = g_pMissShaderTable->GetDesc().Width;
		desc.MissShaderTable.StrideInBytes = desc.MissShaderTable.SizeInBytes;
		desc.RayGenerationShaderRecord.StartAddress = g_pRayGenShaderTable->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = g_pRayGenShaderTable->GetDesc().Width;
		desc.Width = kWindowWidth;
		desc.Height = kWindowHeight;
		dxrCmdList->DispatchRays(g_pDxrPSO_.Get(), &desc);
	}
}

void CopyResultToSwapchain()
{
	auto&& cmdList = g_pCmdLists_[g_frameIndex_];
	auto&& swapchain = g_pSwapchainTex_[g_frameIndex_];

	D3D12_RESOURCE_BARRIER barrier[2]{};

	barrier[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier[0].Transition.pResource = swapchain.Get();
	barrier[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier[1].Transition.pResource = g_pResultOutput_.Get();
	barrier[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	barrier[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cmdList->ResourceBarrier(ARRAYSIZE(barrier), barrier);

	cmdList->CopyResource(swapchain.Get(), g_pResultOutput_.Get());

	barrier[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier[0].Transition.pResource = swapchain.Get();
	barrier[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	barrier[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier[1].Transition.pResource = g_pResultOutput_.Get();
	barrier[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	cmdList->ResourceBarrier(ARRAYSIZE(barrier), barrier);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
	InitWindow(hInstance, nCmdShow);

	if (!InitDevice())
	{
		return -1;
	}

	if (!InitRaytraceDevice())
	{
		return -1;
	}
	if (!InitRaytracePipeline())
	{
		return -1;
	}

	if (!InitGeometry())
	{
		return -1;
	}
	if (!InitAccelerationStructure())
	{
		return -1;
	}
	if (!InitShaderTable())
	{
		return -1;
	}

	// メインループ
	MSG msg = { 0 };
	while (true)
	{
		// Process any messages in the queue.
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);

			if (msg.message == WM_QUIT)
				break;
		}

		auto&& cmdList = g_pCmdLists_[g_frameIndex_];
		cmdList->Reset(g_pCmdAllocator_.Get(), nullptr);

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = g_pSwapchainTex_[g_frameIndex_].Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		cmdList->ResourceBarrier(1, &barrier);

		auto&& rtv = g_swapchainRtv_[g_frameIndex_];
		float color[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
		cmdList->ClearRenderTargetView(rtv.cpu_handle, color, 0, nullptr);

		LetsRaytracing();

		CopyResultToSwapchain();

		cmdList->Close();
		ID3D12CommandList* cmdLists[] = { cmdList.Get() };
		g_pGraphicsQueue_->ExecuteCommandLists(ARRAYSIZE(cmdLists), cmdLists);
		WaitDrawDone();

		g_pSwapchain_->Present(1, 0);
		g_frameIndex_ = g_pSwapchain_->GetCurrentBackBufferIndex();
	}

	WaitDrawDone();

	DestroyShaderTable();
	DestroyAccelerationStructure();
	DestroyGeometry();
	DestroyRaytracePipeline();
	DestroyRaytraceDevice();
	DestroyDevice();

	return static_cast<char>(msg.wParam);
}


