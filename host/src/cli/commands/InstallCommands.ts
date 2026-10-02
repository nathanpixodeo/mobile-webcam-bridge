import { MediaError } from '#domain/errors.ts';
import { describeMode } from '#domain/video/VideoMode.ts';
import { createNativeHelper } from '#composition/createBridgeApp.ts';
import type { InstallResult, NativeHelper } from '#ports/NativeHelper.ts';
import { ExitCode, type Command, type CommandContext } from '../Command.ts';
import type { Terminal } from '../Terminal.ts';

function requireNative(context: CommandContext): NativeHelper {
  const native = createNativeHelper(context);
  if (native === undefined) {
    throw new MediaError(
      'NATIVE_NOT_INSTALLED',
      'bridge-native.exe not found. Build it with scripts/build-native.ps1 first.',
    );
  }
  return native;
}

function report(terminal: Terminal, result: InstallResult): number {
  for (const step of result.steps) {
    terminal.line(
      `${step.ok ? '✔' : '✖'} ${step.name}${step.error === undefined ? '' : `: ${step.error.message} (${step.error.code})`}`,
    );
  }
  if (result.installDir !== undefined) terminal.line(`Installed in ${result.installDir}`);
  return result.ok ? ExitCode.Ok : ExitCode.Failure;
}

/** Installs the virtual camera and microphone (one UAC prompt). */
export class InstallCommand implements Command {
  readonly name = 'install';
  readonly summary = 'Install the virtual camera and microphone (administrator)';
  readonly options = {
    camera: { type: 'string' },
    'no-mic': { type: 'boolean' },
    width: { type: 'string' },
    height: { type: 'string' },
    fps: { type: 'string' },
    'max-width': { type: 'string' },
    'max-height': { type: 'string' },
    'max-fps': { type: 'string' },
  } as const;

  async run(context: CommandContext): Promise<number> {
    const { config, options, terminal } = context;
    const camera = (typeof options['camera'] === 'string' ? options['camera'] : config.camera.backend) as
      'auto' | 'mf' | 'dshow' | 'none';
    const number = (name: string, fallback: number): number =>
      typeof options[name] === 'string' ? Number(options[name]) : fallback;
    terminal.line('Windows will ask for administrator permission…');
    const result = await requireNative(context).install({
      camera,
      mic: options['no-mic'] !== true,
      mode: {
        width: number('width', config.camera.width),
        height: number('height', config.camera.height),
        fps: number('fps', config.camera.fps),
      },
      cap: {
        maxWidth: number('max-width', config.camera.maxWidth),
        maxHeight: number('max-height', config.camera.maxHeight),
        maxFps: number('max-fps', config.camera.maxFps),
      },
      friendlyName: config.camera.friendlyName,
    });
    return report(terminal, result);
  }
}

/** Removes the virtual camera and/or microphone. */
export class UninstallCommand implements Command {
  readonly name = 'uninstall';
  readonly summary = 'Remove the virtual camera and microphone (administrator)';
  readonly options = { camera: { type: 'boolean' }, mic: { type: 'boolean' } } as const;

  async run(context: CommandContext): Promise<number> {
    const { options, terminal } = context;
    const onlyCamera = options['camera'] === true;
    const onlyMic = options['mic'] === true;
    const everything = !onlyCamera && !onlyMic;
    terminal.line('Windows will ask for administrator permission…');
    return report(
      terminal,
      await requireNative(context).uninstall({ camera: everything || onlyCamera, mic: everything || onlyMic }),
    );
  }
}

/** Shows what is installed. */
export class StatusCommand implements Command {
  readonly name = 'status';
  readonly summary = 'Show the installed virtual devices';
  readonly options = {};

  async run(context: CommandContext): Promise<number> {
    const status = await requireNative(context).status();
    const t = context.terminal;
    t.line(`Windows build ${status.osBuild}${status.isWin11 ? ' (Windows 11)' : ''}`);
    t.line(`Install folder: ${status.installDir ?? 'not installed'}`);
    const camera = status.camera;
    if (camera.installed) {
      const { maxWidth, maxHeight, maxFps } = camera.cap;
      t.line(`Camera: ${camera.friendlyName} via ${camera.backend}, default mode ${describeMode(camera.defaultMode)}`);
      t.line(`  Up to ${maxWidth}x${maxHeight} at ${maxFps} fps; modes: ${camera.modes.map(describeMode).join(', ')}`);
    } else {
      t.line('Camera: not installed');
    }
    t.line(
      status.mic.installed
        ? `Microphone: ${status.mic.devicePresent ? 'present' : 'missing'}${status.mic.problemCode === 0 ? '' : ` (device problem code ${status.mic.problemCode})`}`
        : 'Microphone: not installed',
    );
    return ExitCode.Ok;
  }
}
