{ Close only this installed executable. Holding the process handle prevents a
  recycled PID from redirecting termination to an unrelated application. }
function OpenProcess(Access: LongWord; Inherit: Boolean; Id: LongWord): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function QueryFullProcessImageName(Handle: THandle; Flags: LongWord; Name: String; var Size: LongWord): Boolean;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function TerminateProcess(Handle: THandle; Code: LongWord): Boolean;
  external 'TerminateProcess@kernel32.dll stdcall';
function CreateMutex(Attributes: NativeInt; Owner: Boolean; Name: String): THandle;
  external 'CreateMutexW@kernel32.dll stdcall';
function ReleaseMutex(Handle: THandle): Boolean;
  external 'ReleaseMutex@kernel32.dll stdcall';
function EnumWindows(Callback: NativeInt; Parameter: NativeInt): Boolean;
  external 'EnumWindows@user32.dll stdcall';
function GetWindowThreadProcessId(Window: HWND; var Id: LongWord): LongWord;
  external 'GetWindowThreadProcessId@user32.dll stdcall';
function WindowClassName(Window: HWND; Name: String; Size: Integer): Integer;
  external 'GetClassNameW@user32.dll stdcall';
function SendMessageTimeout(Window: HWND; Message: LongWord; WParam: NativeUInt;
  LParam: NativeInt; Flags, Timeout: LongWord; var Reply: NativeUInt): NativeInt;
  external 'SendMessageTimeoutW@user32.dll stdcall';

var
  ClosingProcessId: LongWord;
  ClosingRefused: Boolean;

function CloseEditorWindow(Window: HWND; Parameter: NativeInt): Boolean;
var Id: LongWord; Reply: NativeUInt; Name: String; Size: Integer;
begin
  Result := True;
  GetWindowThreadProcessId(Window, Id);
  if Id <> ClosingProcessId then Exit;
  SetLength(Name, 64); Size := WindowClassName(Window, Name, 64); SetLength(Name, Size);
  if Name <> 'ContourEditor' then Exit;
  { ENDSESSION_CLOSEAPP uses Contour's existing draft recovery, bypassing
    close-to-tray and the interactive Save dialog. A responsive refusal means
    a setup operation or recovery write needs to finish first. }
  Reply := 0;
  if SendMessageTimeout(Window, $11, 0, 1, 2, 2000, Reply) <> 0 then begin
    if Reply = 0 then ClosingRefused := True
    else SendMessageTimeout(Window, $16, 1, 1, 2, 2000, Reply);
  end;
end;

function CloseInstalledEditor: String;
var Locator, Services, Processes, Process: Variant;
    Handle, Mutex: THandle; Name: String; Size, Id, Wait: LongWord; I: Integer;
begin
  Result := '';
#ifdef FixtureBuild
  Mutex := CreateMutex(0, False, 'Local\{#FixtureId}.Deployment');
#else
  Mutex := CreateMutex(0, False, 'Global\ContourLabDeployment');
#endif
  if Mutex = 0 then begin
    Result := 'Windows could not prepare Contour for removal or update. Try again.';
    Exit;
  end;
  Wait := WaitForSingleObject(Mutex, 0);
  if (Wait <> 0) and (Wait <> $80) then begin
    CloseHandle(Mutex);
    Result := 'Contour is finishing an installation or removal. Wait a moment, then try again.';
    Exit;
  end;
  try
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ProcessId, ExecutablePath FROM Win32_Process WHERE Name = ''contour.exe''');
    for I := 0 to Processes.Count - 1 do begin
      Process := Processes.ItemIndex(I);
      if VarIsNull(Process.ExecutablePath) then Continue;
      if CompareText(Process.ExecutablePath, ExpectedDirectory + '\contour.exe') <> 0 then Continue;
      Id := Process.ProcessId;
      Handle := OpenProcess($100001 or $1000, False, Id);
      if Handle = 0 then begin
        Result := 'Contour could not be closed. Try again.';
        Exit;
      end;
      try
        if WaitForSingleObject(Handle, 0) <> 0 then begin
          Size := 32768; SetLength(Name, Size);
          if not QueryFullProcessImageName(Handle, 0, Name, Size) then
            RaiseException('Cannot verify the running Contour executable.');
          SetLength(Name, Size);
          if CompareText(Name, ExpectedDirectory + '\contour.exe') <> 0 then Continue;
          ClosingProcessId := Id; ClosingRefused := False;
          EnumWindows(CreateCallback(@CloseEditorWindow), 0);
          if ClosingRefused then begin
            Result := 'Contour is finishing an operation. Wait a moment, then try again.';
            Exit;
          end;
          if WaitForSingleObject(Handle, 2000) <> 0 then begin
            Log('Closing unresponsive Contour process ' + IntToStr(Id));
            if not TerminateProcess(Handle, 0) then RaiseException('Cannot stop Contour.');
            if WaitForSingleObject(Handle, 5000) <> 0 then RaiseException('Contour did not exit.');
          end;
        end;
      finally
        CloseHandle(Handle);
      end;
    end;
  except
    Log(GetExceptionMessage);
    Result := 'Contour could not be closed. Try again.';
  end;
  finally
    ReleaseMutex(Mutex); CloseHandle(Mutex);
  end;
end;
