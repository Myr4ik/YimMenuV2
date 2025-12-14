#include "Renderer.hpp"
#include "core/memory/ModuleMgr.hpp"
#include "core/memory/PatternScanner.hpp"
#include "game/frontend/GUI.hpp"
#include "game/frontend/Menu.hpp"
#include "game/pointers/Pointers.hpp"
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <vector>
#include <ranges> // Для std::views::values

namespace YimMenu
{
	Renderer::Renderer() :
	    m_Initialized(false),
	    m_Resizing(false),
	    m_FontsUpdated(false),
		m_SafeToRender(false),
		m_FrameIndex(0),
		m_SwapchainWaitableObject(nullptr) // Инициализация нулем обязательна
	{
	}

	Renderer::~Renderer()
	{
		// Гарантируем очистку ресурсов при уничтожении объекта
		DestroyImpl();
	}

	void Renderer::DestroyImpl()
	{
		if (!m_Initialized)
			return;

		ImGui_ImplWin32_Shutdown();

		// Ждем завершения всех операций GPU перед удалением ресурсов
		WaitForLastFrame();
		
		ImGui_ImplDX12_InvalidateDeviceObjects();

		for (size_t i{}; i < m_FrameContext.size(); ++i)
		{
			REL(m_FrameContext[i].Resource);

			// ВАЖНО: m_FrameContext[0].CommandAllocator является сырым указателем на m_CommandAllocator (ComPtr).
			// m_CommandAllocator освободится автоматически деструктором ComPtr.
			// Аллокаторы с индексом > 0 создавались вручную, их нужно освободить явно.
			if (i > 0)
			{
				REL(m_FrameContext[i].CommandAllocator);
			}
		}

		m_FrameContext.clear();

		if (m_FenceEvent)
		{
			CloseHandle(m_FenceEvent);
			m_FenceEvent = nullptr;
		}

		ImGui_ImplDX12_Shutdown();
		ImGui::DestroyContext();
		
		m_Initialized = false;
	}

	bool Renderer::InitDX12()
	{
		if (!Pointers.SwapChain || !Pointers.CommandQueue)
		{
			LOG(WARNING) << "SwapChain or CommandQueue pointers are invalid!";
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

		if (FAILED(m_SwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(m_Device.GetAddressOf()))))
		{
			LOG(WARNING) << "Failed to get D3D Device";
			return false;
		}

		if (FAILED(m_SwapChain->GetDesc(&m_SwapChainDesc)))
		{
			LOG(WARNING) << "Failed to get SwapChain Description";
			return false;
		}

		if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)m_Fence.GetAddressOf())))
		{
			LOG(WARNING) << "Failed to create Fence";
			return false;
		}

		if (m_FenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr); !m_FenceEvent)
		{
			LOG(WARNING) << "Failed to create Fence Event!";
			return false;
		}

		m_FrameContext.resize(m_SwapChainDesc.BufferCount);

		D3D12_DESCRIPTOR_HEAP_DESC DescriptorDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_SwapChainDesc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
		if (FAILED(m_Device->CreateDescriptorHeap(&DescriptorDesc, __uuidof(ID3D12DescriptorHeap), (void**)m_DescriptorHeap.GetAddressOf())))
		{
			LOG(WARNING) << "Failed to create Descriptor Heap";
			return false;
		}

		// Создаем первичный аллокатор (управляется ComPtr)
		if (FAILED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)m_CommandAllocator.GetAddressOf())))
		{
			LOG(WARNING) << "Failed to create primary Command Allocator";
			return false;
		}

		m_FrameContext[0].CommandAllocator = m_CommandAllocator.Get();

		// Создаем вторичные аллокаторы для остальных буферов
		for (size_t i = 1; i < m_SwapChainDesc.BufferCount; ++i)
		{
			if (FAILED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&m_FrameContext[i].CommandAllocator)))
			{
				LOG(WARNING) << "Failed to create secondary Command Allocator";
				return false;
			}
		}

		if (FAILED(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator.Get(), NULL, __uuidof(ID3D12GraphicsCommandList), (void**)m_CommandList.GetAddressOf())))
		{
			LOG(WARNING) << "Failed to create Command List";
			return false;
		}

		if (FAILED(m_CommandList->Close()))
		{
			LOG(WARNING) << "Failed to finalize Command List";
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC DescriptorBackbufferDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, m_SwapChainDesc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
		if (FAILED(m_Device->CreateDescriptorHeap(&DescriptorBackbufferDesc, __uuidof(ID3D12DescriptorHeap), (void**)m_BackbufferDescriptorHeap.GetAddressOf())))
		{
			LOG(WARNING) << "Failed to create Backbuffer Descriptor Heap";
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

		// Если SwapChain был создан с флагом DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, 
		// здесь можно получить хендл. В большинстве случаев хуков это null.
		// m_SwapchainWaitableObject = m_SwapChain->GetFrameLatencyWaitableObject();

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
		// Если окно свернуто (размер 0), не рисуем, чтобы избежать ошибок и зависаний.
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
		// Используем ссылку FrameContext&, чтобы изменять fence value в реальном объекте, а не в копии
		FrameContext& FrameCtx = GetInstance().m_FrameContext[GetInstance().m_FrameIndex % GetInstance().m_SwapChainDesc.BufferCount];
		UINT64 FenceValue = FrameCtx.FenceValue;

		if (FenceValue == 0) return;

		FrameCtx.FenceValue = 0;

		if (GetInstance().m_Fence->GetCompletedValue() >= FenceValue) return;

		GetInstance().m_Fence->SetEventOnCompletion(FenceValue, GetInstance().m_FenceEvent);
		WaitForSingleObject(GetInstance().m_FenceEvent, INFINITE);
	}

	void Renderer::WaitForNextFrame()
	{
		auto& inst = GetInstance();
		UINT NextFrameIndex = inst.m_FrameIndex + 1;
		inst.m_FrameIndex = NextFrameIndex;

		HANDLE WaitableObjects[2] = { nullptr, nullptr };
		DWORD NumWaitableObjects = 0;

		// Добавляем Swapchain Waitable Object только если он валиден
		if (inst.m_SwapchainWaitableObject)
		{
			WaitableObjects[NumWaitableObjects++] = inst.m_SwapchainWaitableObject;
		}

		// Используем ссылку FrameContext&
		FrameContext& FrameCtx = inst.m_FrameContext[NextFrameIndex % inst.m_SwapChainDesc.BufferCount];
		UINT64 FenceValue = FrameCtx.FenceValue;
		
		if (FenceValue != 0) 
		{
			FrameCtx.FenceValue = 0;
			inst.m_Fence->SetEventOnCompletion(FenceValue, inst.m_FenceEvent);
			WaitableObjects[NumWaitableObjects++] = inst.m_FenceEvent;
		}

		if (NumWaitableObjects > 0)
		{
			WaitForMultipleObjects(NumWaitableObjects, WaitableObjects, TRUE, INFINITE);
		}
	}

	void Renderer::DX12PreResize()
	{
		SetResizing(true);
		
		WaitForLastFrame();

		// Освобождаем только ресурсы (Buffers), аллокаторы команд оставляем
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
		
		renderer.m_SwapChain->GetDesc(&renderer.m_SwapChainDesc);

		// Проверка на свернутое окно (0x0)
		if (renderer.m_SwapChainDesc.BufferDesc.Width == 0 || 
			renderer.m_SwapChainDesc.BufferDesc.Height == 0 || 
			renderer.m_SwapChainDesc.BufferCount == 0)
		{
			SetResizing(false);
			return;
		}

		// Если изменилось количество буферов
		if (renderer.m_FrameContext.size() != renderer.m_SwapChainDesc.BufferCount)
		{
			renderer.m_FrameContext.resize(renderer.m_SwapChainDesc.BufferCount);
			
			// Создаем недостающие аллокаторы
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

		// Проверка: поместятся ли новые буферы в текущую кучу RTV дескрипторов
		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = renderer.m_BackbufferDescriptorHeap->GetDesc();
		if (renderer.m_SwapChainDesc.BufferCount > rtvDesc.NumDescriptors)
		{
			// Если нет, пересоздаем кучу
			renderer.m_BackbufferDescriptorHeap.Reset();
			D3D12_DESCRIPTOR_HEAP_DESC DescriptorBackbufferDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, renderer.m_SwapChainDesc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
			renderer.m_Device->CreateDescriptorHeap(&DescriptorBackbufferDesc, __uuidof(ID3D12DescriptorHeap), (void**)renderer.m_BackbufferDescriptorHeap.GetAddressOf());
		}

		const auto RTVDescriptorSize{renderer.m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
		D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle{renderer.m_BackbufferDescriptorHeap->GetCPUDescriptorHandleForHeapStart()};
		
		for (size_t i{}; i != renderer.m_SwapChainDesc.BufferCount; ++i)
		{
			ComPtr<ID3D12Resource> BackBuffer{};
			
			renderer.m_FrameContext[i].Resource = nullptr;
			renderer.m_FrameContext[i].FenceValue = 0; 

			if (SUCCEEDED(renderer.m_SwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)BackBuffer.GetAddressOf())))
			{
				renderer.m_Device->CreateRenderTargetView(BackBuffer.Get(), nullptr, RTVHandle);
				renderer.m_FrameContext[i].Resource = BackBuffer.Get();
				renderer.m_FrameContext[i].Descriptor = RTVHandle;
			}
			
			RTVHandle.ptr += RTVDescriptorSize;
		}

		renderer.m_FrameIndex = 0;
		// ImGui_ImplWin32_NewFrame автоматически обновит DisplaySize, вручную это делать не нужно.

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
