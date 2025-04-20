@rem FidelityFX SDKのSPDについてライブラリビルドするためのbatファイルです
@rem
@rem ==== CONFIGURE ====
@cd %~dp0\FidelityFX-SDK\sdk

BuildFidelityFXSDK.bat -DFFX_API_BACKEND=DX12_X64 -DFFX_AUTO_COMPILE_SHADERS=1 -DFFX_SPD=ON

pause
