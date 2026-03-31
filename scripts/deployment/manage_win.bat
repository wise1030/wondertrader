@echo off
setlocal

cd /d "%~dp0"

if "%1"=="build" goto build
if "%1"=="deploy" goto deploy
if "%1"=="start" goto start
if "%1"=="all" goto all

echo Usage: %0 {build^|deploy^|start^|all}
exit /b 1

:all
call :build
call :deploy
call :start
goto :eof

:build
echo [INFO] Starting Build Process...
where msbuild >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] MSBuild not found in PATH. Please run this script from Visual Studio Developer Command Prompt.
    exit /b 1
)

cd ..\..\src
msbuild all.sln /t:Build /p:Configuration=Release /p:Platform=x64
if %errorlevel% neq 0 (
    echo [ERROR] Build Failed!
    cd ..\scripts\deployment
    exit /b 1
)
echo [INFO] Build Successful!
cd ..\scripts\deployment
goto :eof

:deploy
echo [INFO] Deploying WonderTrader...
set SOURCE_DIR=..\..\src\x64\Release
set DEPLOY_DIR=..\..\dist

if not exist "%DEPLOY_DIR%" mkdir "%DEPLOY_DIR%"
if not exist "%DEPLOY_DIR%\bin" mkdir "%DEPLOY_DIR%\bin"
if not exist "%DEPLOY_DIR%\config" mkdir "%DEPLOY_DIR%\config"
if not exist "%DEPLOY_DIR%\logs" mkdir "%DEPLOY_DIR%\logs"
if not exist "%DEPLOY_DIR%\data" mkdir "%DEPLOY_DIR%\data"

copy "%SOURCE_DIR%\WtRunner.exe" "%DEPLOY_DIR%\bin\"
copy "%SOURCE_DIR%\WtBtRunner.exe" "%DEPLOY_DIR%\bin\"
copy "%SOURCE_DIR%\WtCore.dll" "%DEPLOY_DIR%\bin\"
copy "%SOURCE_DIR%\WtDtCore.dll" "%DEPLOY_DIR%\bin\"
copy "%SOURCE_DIR%\WtPorter.dll" "%DEPLOY_DIR%\bin\"

echo [INFO] Copying Plugins...
for %%f in ("%SOURCE_DIR%\Parser*.dll") do copy "%%f" "%DEPLOY_DIR%\bin\"
for %%f in ("%SOURCE_DIR%\Trader*.dll") do copy "%%f" "%DEPLOY_DIR%\bin\"
for %%f in ("%SOURCE_DIR%\Wt*Fact.dll") do copy "%%f" "%DEPLOY_DIR%\bin\"

echo @echo off > "%DEPLOY_DIR%\start.bat"
echo cd /d %%~dp0 >> "%DEPLOY_DIR%\start.bat"
echo cd bin >> "%DEPLOY_DIR%\start.bat"
echo WtRunner.exe -c ../config/config.yaml -l ../config/logcfg.yaml >> "%DEPLOY_DIR%\start.bat"
echo pause >> "%DEPLOY_DIR%\start.bat"

echo [INFO] Deployment Completed.
echo [INFO] Config files need to be manually copied to %DEPLOY_DIR%\config\
goto :eof

:start
echo [INFO] Starting WtRunner...
cd ..\..\dist\bin
if not exist "WtRunner.exe" (
    echo [ERROR] WtRunner.exe not found in bin directory!
    cd ..\..\scripts\deployment
    exit /b 1
)
echo [INFO] Using Config: ../config/config.yaml
WtRunner.exe -c ../config/config.yaml -l ../config/logcfg.yaml
cd ..\..\scripts\deployment
goto :eof
