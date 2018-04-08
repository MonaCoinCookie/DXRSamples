struct SceneCB
{
	float4x4	mtxProjToWorld;
	float4		camPos;
	float4		lightDir;
	float4		lightColor;
};

struct InstanceCB
{
	float4		quatRot;
	float4		matColor;
};

struct Vertex
{
	float3		pos;
	float3		normal;
};

struct HitData
{
	float4 color;
};

RaytracingAccelerationStructure		Scene			: register(t0, space0);
ByteAddressBuffer					Indices			: register(t1, space0);
StructuredBuffer<Vertex>			Vertices		: register(t2, space0);
RWTexture2D<float4>					RenderTarget	: register(u0);
ConstantBuffer<SceneCB>				cbScene			: register(b0);

ConstantBuffer<InstanceCB>			cbInstance		: register(b1);

// 2�o�C�g�̎O�p�`�C���f�b�N�X���擾����
// ByteAddressBuffer��4�o�C�g�A���C�������g�ŁA4�o�C�g������Load�ł��Ȃ����߁A����Ȗ��߂𗘗p����
uint3 GetTriangleIndices2byte(uint offset)
{
	uint alignedOffset = offset & ~0x3;
	uint2 indices4 = Indices.Load2(alignedOffset);

	uint3 ret;
	if (alignedOffset == offset)
	{
		ret.x = indices4.x & 0xffff;
		ret.y = (indices4.x >> 16) & 0xffff;
		ret.z = indices4.y & 0xffff;
	}
	else
	{
		ret.x = (indices4.x >> 16) & 0xffff;
		ret.y = indices4.y & 0xffff;
		ret.z = (indices4.y >> 16) & 0xffff;
	}

	return ret;
}

// �N�I�[�^�j�I�������]�s������߂�
float3 RotVectorByQuat(float3 v, float4 q)
{
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}


[shader("raygeneration")]
void RayGenerator()
{
	// �s�N�Z�����S���W���N���b�v��ԍ��W�ɕϊ�
	uint2 index = DispatchRaysIndex();
	float2 xy = (float2)index + 0.5;
	float2 clipSpacePos = xy / DispatchRaysDimensions() * float2(2, -2) + float2(-1, 1);

	// �N���b�v��ԍ��W�����[���h��ԍ��W�ɕϊ�
	float4 worldPos = mul(float4(clipSpacePos, 0, 1), cbScene.mtxProjToWorld);

	// ���[���h��ԍ��W�ƃJ�����ʒu���烌�C�𐶐�
	worldPos.xyz /= worldPos.w;
	float3 origin = cbScene.camPos.xyz;
	float3 direction = normalize(worldPos.xyz - origin);

	// Let's ���C�g���I
	RayDesc ray = { origin, 0.0f, direction, 10000.0f };
	HitData payload = { float4(0, 0, 0, 0) };
	TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);

	// Write the raytraced color to the output texture.
	RenderTarget[index] = payload.color;
}

[shader("closesthit")]
void ClosestHitProcessor(inout HitData payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attr : SV_IntersectionAttributes)
{
	// �q�b�g�����v���~�e�B�u�C���f�b�N�X����g���C�A���O���̒��_�C���f�b�N�X�����߂�
	uint indexOffset = PrimitiveIndex() * 2 * 3;
	uint3 indices = GetTriangleIndices2byte(indexOffset);

	// �q�b�g�ʒu�̖@�������߂�
	float3 vertexNormals[3] = {
		Vertices[indices.x].normal,
		Vertices[indices.y].normal,
		Vertices[indices.z].normal
	};
	float3 normal = vertexNormals[0] +
		attr.barycentrics.x * (vertexNormals[1] - vertexNormals[0]) +
		attr.barycentrics.y * (vertexNormals[2] - vertexNormals[0]);
	normal = normalize(normal);

	// �@�������[���h��Ԃɕϊ�����
	normal = RotVectorByQuat(normal, cbInstance.quatRot);

	// ���s�����̃��C�e�B���O�v�Z
	float NoL = saturate(dot(normal, -cbScene.lightDir.xyz));
	float3 finalColor = cbInstance.matColor * cbScene.lightColor.rgb * NoL;

	float3 addColor = float3(0, 0, 0);
	if (!(RayFlags() & RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH))
	{
		float3 origin = WorldRayOrigin() + WorldRayDirection() * RayTCurrent() + normal * 1e-4;
		float3 reflection = dot(normal, -WorldRayDirection()) * 2.0 * normal + WorldRayDirection();
		RayDesc ray = { origin, 0.0f, reflection, 10000.0f };
		HitData reflPayload = { float4(0, 0, 0, 0) };
		TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, ~0, 0, 1, 0, ray, reflPayload);

		addColor = reflPayload.color.rgb * 0.2f;
	}

	payload.color = float4(finalColor + addColor, 1);
}

[shader("miss")]
void MissProcessor(inout HitData payload : SV_RayPayload)
{
	payload.color = float4(0, 0, 1, 1);
}

// EOF
