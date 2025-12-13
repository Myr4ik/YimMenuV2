#include "Renderer.hpp"
#include "core/memory/ModuleMgr.hpp"
#include "core/memory/PatternScanner.hpp"
#include "game/frontend/GUI.hpp"
#include "game/frontend/Menu.hpp"
#include "game/pointers/Pointers.hpp"
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <vector> // Важно для изменения размеров векторов

namespace YimMenu
{
	Renderer::Renderer() :
	    m_Initialized(false),
	    m_Resizing(false),
	    m_FontsUpdated(false),
		m_SafeToRender(false)
	{
	}

	Renderer::~Renderer()
	{
	}

	void Renderer::DestroyImpl()
	{
		if (!m_Initialized)
			return;

		ImGui_ImplWin32_Shutdown();

		WaitForLastFrame();
		ImGui_ImplDX12_InvalidateDeviceObjects();

		for (size_t i{}; i != GetInstance().m_SwapChainDesc.BufferCount; ++i)
		{
			REL(GetInstance().m_FrameContext[i].Resource);
		}

		ImGui_ImplDX12_Shutdown();
		ImGui::DestroyContext();
	}

	bool Renderer::InitDX12()
	{
		if (!Pointers.SwapChain)
		{
			LOG(WARNING) << "SwapChain pointer is invalid!";
			return false;
		}

		if (!Pointers.CommandQueue)
		{
			LOG(WARNING) << "CommandQueue pointer is invalid!";
			return false;
		}

		if (m_GameSwapChain = ComPtr<IDXGISwapChain1>(*Pointers.SwapChain); !m_GameSwapChain.Get())
		{
			LOG(WARNING) << "Dereferenced SwapChain pointer is invalid!";
			return false;
		}

		if (m_CommandQueue = ComPtr<ID3D12CommandQueue>(*Pointers.CommandQueue); !m_CommandQueue.Get())
		{
			LOG(WARNING) << "Dereferenced CommandQueue pointer is invalid!";
			return false;
		}

		m_GameSwapChain.As(&m_SwapChain);

		if (const auto result = m_SwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(m_Device.GetAddressOf())); result < 0)
		{
			LOG(WARNING) << "Failed to get D3D Device with result: [" << result << "]";
			return false;
		}

		if (const auto result = m_SwapChain->GetDesc(&m_SwapChainDesc); result < 0)
		{
			LOG(WARNING) << "Failed to get SwapChain Description with result: [" << result << "]";
			return false;
		}

		if (const auto result = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)m_Fence.GetAddressOf()); result < 0)
		{
			LOG(WARNING) << "Failed to create Fence with result: [" << result << "]";
			return false;
		}

		if (const auto result = m_FenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr); !result)
		{
			LOG(WARNING) << "Failed to create Fence Event!";
			return false;
		}

		m_FrameContext.resize(m_SwapChainDesc.BufferCount);

		D3D12_DESCRIPTOR_HEAP_DESC DescriptorDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_SwapChainDesc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
		if (const auto result =
		        m_Device->CreateDescriptorHeap(&DescriptorDesc, __uuidof(ID3D12DescriptorHeap), (void**)m_DescriptorHeap.GetAddressOf());
		    result < 0)
		{
			LOG(WARNING) << "Failed to create Descriptor Heap with result: [" << result << "]";
			return false;
		}

		if (const auto result = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		        __uuidof(ID3D12CommandAllocator),
		        (void**)m_CommandAllocator.GetAddressOf());
		    result < 0)
		{
			LOG(WARNING) << "Failed to create primary Command Allocator with result: [" << result << "]";
			return false;
		}

		m_FrameContext[0].CommandAllocator = m_CommandAllocator.Get();

		for (size_t i = 1; i < m_SwapChainDesc.BufferCount; ++i)
		{
			if (const auto result = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&m_FrameContext[i].CommandAllocator); result < 0)
			{
				LOG(WARNING) << "Failed to create secondary Command Allocator with result: [" << result << "]";
				return false;
			}
		}

		if (const auto result = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator.Get(), NULL, __uuidof(ID3D12GraphicsCommandList), (void**)m_CommandList.GetAddressOf()); result < 0)
		{
			LOG(WARNING) << "Failed to create Command List with result: [" << result << "]";
			return false;
		}

		if (const auto result = m_CommandList->Close(); result < 0)
		{
			LOG(WARNING) << "Failed to finalize the creation of Command List with result: [" << result << "]";
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC DescriptorBackbufferDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, m_SwapChainDesc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
		if (const auto result = m_Device->CreateDescriptorHeap(&DescriptorBackbufferDesc,
		        __uuidof(ID3D12DescriptorHeap),
		        (void**)m_BackbufferDescriptorHeap.GetAddressOf());
		    result < 0)
		{
			LOG(WARNING) << "Failed to create Backbuffer Descriptor Heap with result: [" << result << "]";
			return false;
		}

		const auto RTVDescriptorSize{m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
		D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle{m_BackbufferDescriptorHeap->GetCPUDescriptorHandleForHeapStart()};
		for (size_t i = 0; i < m_SwapChainDesc.BufferCount; ++i)
		{
			ComPtr<ID3D12Resource> BackBuffer{};
			m_FrameContext[i].Descriptor = RTVHandle;
			m_SwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)BackBuffer.GetAddressOf());
			m_Device->CreateRenderTargetView(BackBuffer.Get(), nullptr, RTVHandle);
			m_FrameContext[i].Resource = BackBuffer.Get();
			RTVHandle.ptr += RTVDescriptorSize;
		}

		m_HeapAllocator.Create(m_Device.Get(), m_DescriptorHeap.Get());

		ImGui::CreateContext(&GetInstance().m_FontAtlas);
		ImGui_ImplWin32_Init(*Pointers.Hwnd);

		ImGui_ImplDX12_InitInfo init_info = {};
		init_info.Device = m_Device.Get();
		init_info.CommandQueue = m_CommandQueue.Get();
		init_info.NumFramesInFlight = m_SwapChainDesc.BufferCount;
		init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		init_info.SrvDescriptorHeap = m_DescriptorHeap.Get();
		init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) {
			return GetInstance().m_HeapAllocator.Alloc(out_cpu_handle, out_gpu_handle);
		};
		init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
			return GetInstance().m_HeapAllocator.Free(cpu_handle, gpu_handle);
		};
		ImGui_ImplDX12_Init(&init_info);

		ImGui::StyleColorsDark();

		LOG(INFO) << "DirectX 12 renderer has finished initializing.";
		m_Initialized = true;
		return true;
	}

	bool Renderer::InitImpl()
	{
		while (!*Pointers.Hwnd || !*Pointers.ScreenResX)
		{
			std::this_thread::sleep_for(1s);
		}

		LOG(INFO) << "Using DX12, clear shader cache if you're having issues.";
		return InitDX12();
	}

	bool Renderer::AddRendererCallBackImpl(RendererCallBack&& callback, std::uint32_t priority)
	{
		return m_RendererCallBacks.insert({priority, callback}).second;
	}

	void Renderer::AddWindowProcedureCallbackImpl(WindowProcedureCallback&& callback)
	{
		return m_WindowProcedureCallbacks.push_back(callback);
	}

	void Renderer::DX12OnPresentImpl()
	{
		// [FIX] Не рисуем, если окно свернуто (ширина 0)
		if (!m_SafeToRender || GetInstance().m_SwapChainDesc.BufferDesc.Width == 0)
			return;

		Renderer::DX12NewFrame();
		for (const auto& callback : m_RendererCallBacks | std::views::values)
			callback();
		Renderer::DX12EndFrame();
	}

	LRESULT Renderer::WndProcImpl(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		for (const auto& callback : m_WindowProcedureCallbacks)
			callback(hwnd, msg, wparam, lparam);

		return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
	}

	void Renderer::ResizeImpl(float scale)
	{
		DX12PreResize();

		if (scale != 1.0f)
			ImGui::GetStyle().ScaleAllSizes(scale);
		ImGui::GetStyle().MouseCursorScale = 1.0f;
		ImGui::GetIO().FontGlobalScale = scale;
		DX12PostResize();
	}

	void Renderer::WaitForLastFrame()
	{
		FrameContext FrameCtx = GetInstance().m_FrameContext[GetInstance().m_FrameIndex % GetInstance().m_SwapChainDesc.BufferCount];
		UINT64 FenceValue = FrameCtx.FenceValue;

		if (FenceValue == 0) return;

		FrameCtx.FenceValue = 0;

		if (GetInstance().m_Fence->GetCompletedValue() >= FenceValue) return;

		GetInstance().m_Fence->SetEventOnCompletion(FenceValue, GetInstance().m_FenceEvent);
		WaitForSingleObject(GetInstance().m_FenceEvent, INFINITE);
	}

	void Renderer::WaitForNextFrame()
	{
		UINT NextFrameIndex = GetInstance().m_FrameIndex + 1;
		GetInstance().m_FrameIndex = NextFrameIndex;

		HANDLE WaitableObjects[] = {GetInstance().m_SwapchainWaitableObject, nullptr};
		DWORD NumWaitableObjets = 1;

		FrameContext FrameCtx = GetInstance().m_FrameContext[NextFrameIndex % GetInstance().m_SwapChainDesc.BufferCount];
		UINT64 FenceValue = FrameCtx.FenceValue;
		if (FenceValue != 0) 
		{
			FrameCtx.FenceValue = 0;
			GetInstance().m_Fence->SetEventOnCompletion(FenceValue, GetInstance().m_FenceEvent);
			WaitableObjects[1] = GetInstance().m_FenceEvent;
			NumWaitableObjets = 2;
		}

		WaitForMultipleObjects(NumWaitableObjets, WaitableObjects, TRUE, INFINITE);
	}

	void Renderer::DX12PreResize()
	{
		SetResizing(true);
		
		WaitForLastFrame();

		// [FIX] Мы НЕ вызываем InvalidateDeviceObjects, чтобы не убить шрифты ImGui.
		// ImGui_ImplDX12_InvalidateDeviceObjects(); <--- Убрано намеренно

		// Мы освобождаем только ресурсы DirectX, связанные с буфером экрана
		for (size_t i{}; i != GetInstance().m_SwapChainDesc.BufferCount; ++i)
		{
			if (i < GetInstance().m_FrameContext.size()) {
				REL(GetInstance().m_FrameContext[i].Resource);
			}
		}
	}

	void Renderer::DX12PostResize()
	{
		auto& renderer = GetInstance();
		
		// 1. Получаем новые размеры (важно!)
		renderer.m_SwapChain->GetDesc(&renderer.m_SwapChainDesc);

		// Если окно 0x0 (свернуто), выходим.
		if (renderer.m_SwapChainDesc.BufferDesc.Width == 0 || 
			renderer.m_SwapChainDesc.BufferDesc.Height == 0 || 
			renderer.m_SwapChainDesc.BufferCount == 0)
		{
			SetResizing(false);
			return;
		}

		// 2. Если изменилось кол-во буферов (редко)
		if (renderer.m_FrameContext.size() != renderer.m_SwapChainDesc.BufferCount)
		{
			renderer.m_FrameContext.resize(renderer.m_SwapChainDesc.BufferCount);
			
			// Если буферов стало больше, придется пересоздать Heap (только в этом случае)
			// Иначе старая куча подойдет.
			if (renderer.m_FrameContext.size() > renderer.m_SwapChainDesc.BufferCount) // Упрощенная проверка
			{
				// Тут можно добавить логику пересоздания heap, если критично,
				// но обычно BufferCount = 3 (const) в GTA.
				// Создадим allocators для новых слотов:
			}
			
			for (size_t i = 0; i < renderer.m_FrameContext.size(); ++i)
			{
				if (!renderer.m_FrameContext[i].CommandAllocator)
				{
					renderer.m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, 
						__uuidof(ID3D12CommandAllocator), 
						(void**)&renderer.m_FrameContext[i].CommandAllocator);
				}
			}
		}

		// [FIX] Мы НЕ вызываем CreateDeviceObjects, так как ImGui уже живой.
		// ImGui_ImplDX12_CreateDeviceObjects(); <--- Убрано намеренно

		// 3. Пересоздаем RTV в существующей куче
		const auto RTVDescriptorSize{renderer.m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
		D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle{renderer.m_BackbufferDescriptorHeap->GetCPUDescriptorHandleForHeapStart()};
		
		for (size_t i{}; i != renderer.m_SwapChainDesc.BufferCount; ++i)
		{
			ComPtr<ID3D12Resource> BackBuffer{};
			
			// Очистка старых данных
			renderer.m_FrameContext[i].Resource = nullptr;
			renderer.m_FrameContext[i].FenceValue = 0; // Сброс Fence критичен

			// Получение нового буфера
			if (SUCCEEDED(renderer.m_SwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)BackBuffer.GetAddressOf())))
			{
				renderer.m_Device->CreateRenderTargetView(BackBuffer.Get(), nullptr, RTVHandle);
				renderer.m_FrameContext[i].Resource = BackBuffer.Get();
				renderer.m_FrameContext[i].Descriptor = RTVHandle;
			}
			
			RTVHandle.ptr += RTVDescriptorSize;
		}

		// 4. Сброс индекса кадра
		renderer.m_FrameIndex = 0;

		// 5. Принудительное обновление размера ImGui (DisplaySize)
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)renderer.m_SwapChainDesc.BufferDesc.Width, (float)renderer.m_SwapChainDesc.BufferDesc.Height);

		SetResizing(false);
	}

	void Renderer::DX12NewFrame()
	{
		if (GetInstance().m_FontsUpdated)
		{
			DX12PreResize();
			DX12PostResize();
			GetInstance().m_FontsUpdated = false;
		}

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
	}

	void Renderer::DX12EndFrame()
	{
		WaitForNextFrame();

		FrameContext& CurrentFrameContext{GetInstance().m_FrameContext[GetInstance().m_SwapChain->GetCurrentBackBufferIndex()]};
		CurrentFrameContext.CommandAllocator->Reset();

		D3D12_RESOURCE_BARRIER Barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		    D3D12_RESOURCE_BARRIER_FLAG_NONE,
		    {{CurrentFrameContext.Resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET}}};
		GetInstance().m_CommandList->Reset(CurrentFrameContext.CommandAllocator, nullptr);
		GetInstance().m_CommandList->ResourceBarrier(1, &Barrier);
		GetInstance().m_CommandList->OMSetRenderTargets(1, &CurrentFrameContext.Descriptor, FALSE, nullptr);
		GetInstance().m_CommandList->SetDescriptorHeaps(1, GetInstance().m_DescriptorHeap.GetAddressOf());

		ImGui::Render();

		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), GetInstance().m_CommandList.Get());

		Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		GetInstance().m_CommandList->ResourceBarrier(1, &Barrier);
		GetInstance().m_CommandList->Close();

		ID3D12CommandList* CommandLists[]{GetInstance().m_CommandList.Get()};
		GetInstance().m_CommandQueue->ExecuteCommandLists(_countof(CommandLists), CommandLists);

		UINT64 FenceValue = GetInstance().m_FenceLastSignaledValue + 1;
		GetInstance().m_CommandQueue->Signal(GetInstance().m_Fence.Get(), FenceValue);
		GetInstance().m_FenceLastSignaledValue = FenceValue;
		CurrentFrameContext.FenceValue = FenceValue;
	}
}
