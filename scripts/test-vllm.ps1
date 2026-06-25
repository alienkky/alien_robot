Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ModelUrl = "http://localhost:8000/v1/models"
$ContainerName = "aa-vllm-qwen36"

Write-Host "== Docker container status =="
docker ps --filter "name=$ContainerName" --format "table {{.Names}}`t{{.Status}}`t{{.Ports}}"

Write-Host ""
Write-Host "== Docker memory limit =="
docker stats $ContainerName --no-stream

Write-Host ""
Write-Host "== GPU status =="
nvidia-smi

Write-Host ""
Write-Host "== vLLM models endpoint =="
try {
    curl.exe -sS $ModelUrl
    Write-Host ""
    Write-Host "vLLM /v1/models request completed."
} catch {
    Write-Host ""
    Write-Host "vLLM /v1/models request failed."
    Write-Host "If this shows 'Empty reply from server', vLLM is still loading or failed before readiness."
    throw
}

Write-Host ""
Write-Host "== Recent vLLM logs =="
docker logs --tail 80 $ContainerName
