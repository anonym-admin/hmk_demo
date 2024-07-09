#pragma once

using namespace DirectX;

struct Vertex
{
    Vertex() : position(0.0f, 0.0f, 0.0f), normal(0.0f, 0.0f, 0.0f), texCoord(0.0f, 0.0f)
    {
    }
    Vertex(const XMFLOAT3 &p, const XMFLOAT3 &n, const XMFLOAT2 &t) : position(p), normal(n), texCoord(t)
    {
    }
    Vertex(float px, float py, float pz, float nx, float ny, float nz, float tx, float ty)
        : position(px, py, pz), normal(nx, ny, nz), texCoord(tx, ty)
    {
    }

    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 texCoord;
};

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;

    std::string albedoTextureFilename = "";
};

struct Mesh
{
    ID3D12Resource *vertexBuffer = nullptr;
    ID3D12Resource *indexBuffer  = nullptr;
    uint32_t vertexCount         = 0;
    uint32_t indexCount          = 0;
    // Teture
    ID3D12Resource *albedoTexture       = nullptr;
    ID3D12DescriptorHeap *albedoSRVheap = nullptr;

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView()
    {
        D3D12_VERTEX_BUFFER_VIEW view;
        view.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        view.StrideInBytes  = sizeof(Vertex);
        view.SizeInBytes    = vertexCount * view.StrideInBytes;
        return view;
    }

    D3D12_INDEX_BUFFER_VIEW IndexBufferView()
    {
        D3D12_INDEX_BUFFER_VIEW view;
        view.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        view.SizeInBytes    = indexCount * sizeof(uint16_t);
        view.Format         = DXGI_FORMAT_R16_UINT;
        return view;
    }
};