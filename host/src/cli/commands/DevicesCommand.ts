import { toError } from '#domain/errors.ts';
import {
  createDeviceInfrastructure,
  createSessionFactory,
  type DeviceInfrastructure,
} from '#composition/createBridgeApp.ts';
import type { MobileDevice, PendingDevice } from '#ports/DeviceWatcher.ts';
import { ExitCode, type Command, type CommandContext } from '../Command.ts';

/** Lists connected phones (iPhone and Android), whether they are usable, and whether the app answers. */
export class DevicesCommand implements Command {
  readonly name = 'devices';
  readonly summary = 'List connected phones and check the companion app';
  readonly options = {};

  async run(context: CommandContext): Promise<number> {
    const { terminal } = context;
    const infrastructure = createDeviceInfrastructure(context);
    await using watcher = infrastructure.watcher;
    await watcher.start();
    const devices = watcher.devices();
    const pending = watcher.pending();
    if (devices.length === 0 && pending.length === 0) {
      terminal.line('No phone found.');
      terminal.line('  iPhone:  connect it with a USB cable, unlock it and tap "Trust".');
      terminal.line('  Android: enable USB debugging, connect it and accept "Allow USB debugging?".');
      return ExitCode.Ok;
    }
    const rows: string[][] = [];
    for (const device of devices) {
      rows.push([
        platformLabel(device.platform),
        device.id,
        device.link,
        await this.#readiness(device, infrastructure),
        await this.#probeApp(context, device, infrastructure),
      ]);
    }
    for (const device of pending)
      rows.push([platformLabel(device.platform), device.id, 'usb', pendingHint(device), '-']);
    terminal.table(['Phone', 'ID', 'Link', 'State', 'Mobile Webcam app'], rows);
    return ExitCode.Ok;
  }

  async #readiness(device: MobileDevice, infrastructure: DeviceInfrastructure): Promise<string> {
    if (device.transport !== 'usbmux' || infrastructure.usbmux === undefined) return 'ready';
    const paired = await infrastructure.usbmux.isPaired(device.id).catch(() => undefined);
    if (paired === undefined) return 'ready (trust unknown)';
    return paired ? 'ready (trusted)' : 'not trusted: tap Trust on the iPhone';
  }

  async #probeApp(
    context: CommandContext,
    device: MobileDevice,
    infrastructure: DeviceInfrastructure,
  ): Promise<string> {
    await using session = createSessionFactory(context, infrastructure.tunnels)(device);
    session.start();
    const deadline =
      context.clock.nowMs() + context.config.device.connectTimeoutMs + context.config.device.handshakeTimeoutMs;
    try {
      while (context.clock.nowMs() < deadline) {
        const state = session.state;
        if (state.kind === 'ready') {
          return `${state.peer.app.name} ${state.peer.app.version} (protocol ${state.peer.protocol.major}.${state.peer.protocol.minor})`;
        }
        if (state.kind === 'stalled') return `error: ${state.reason.message}`;
        if (state.kind === 'backoff' && state.reason.code === 'APP_NOT_REACHABLE') return 'not running (open the app)';
        await context.clock.sleep(50);
      }
      return 'no answer';
    } catch (error) {
      return `error: ${toError(error).message}`;
    }
  }
}

function platformLabel(platform: MobileDevice['platform']): string {
  switch (platform) {
    case 'ios':
      return 'iPhone';
    case 'android':
      return 'Android';
    case 'unknown':
      return 'device';
  }
}

function pendingHint(device: PendingDevice): string {
  switch (device.state) {
    case 'unauthorized':
      return 'unauthorized: accept "Allow USB debugging?" on the phone';
    case 'offline':
      return 'offline: reconnect the cable';
    default:
      return device.state;
  }
}
