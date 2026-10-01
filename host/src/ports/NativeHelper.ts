import type { VideoMode } from '#domain/video/VideoMode.ts';

export type CameraBackend = 'mf' | 'dshow';

export type CameraStatus =
  | { readonly installed: false }
  | {
      readonly installed: true;
      readonly backend: CameraBackend | 'none';
      readonly friendlyName: string;
      readonly mode: VideoMode;
      readonly pipeName: string;
    };

export type MicStatus =
  | { readonly installed: false }
  | { readonly installed: true; readonly devicePresent: boolean; readonly problemCode: number };

export interface NativeStatus {
  readonly installDir: string | null;
  readonly osBuild: number;
  readonly isWin11: boolean;
  readonly camera: CameraStatus;
  readonly mic: MicStatus;
}

export interface InstallOptions {
  readonly camera: 'auto' | CameraBackend | 'none';
  readonly mic: boolean;
  readonly mode: Pick<VideoMode, 'width' | 'height'> & { readonly fps: number };
  readonly friendlyName: string;
}

export interface InstallStepResult {
  readonly name: string;
  readonly ok: boolean;
  readonly error?: { readonly code: string; readonly message: string } | undefined;
}

export interface InstallResult {
  readonly ok: boolean;
  readonly installDir: string | undefined;
  readonly steps: readonly InstallStepResult[];
}

export interface DoctorCheckResult {
  readonly id: string;
  readonly status: 'pass' | 'warn' | 'fail';
  readonly message: string;
}

/** One-shot operations of bridge-native.exe (protocol/BRIDGE_NATIVE.md §2). */
export interface NativeHelper {
  readonly executablePath: string;
  status(): Promise<NativeStatus>;
  install(options: InstallOptions): Promise<InstallResult>;
  uninstall(scope: { readonly camera: boolean; readonly mic: boolean }): Promise<InstallResult>;
  doctor(): Promise<readonly DoctorCheckResult[]>;
}
