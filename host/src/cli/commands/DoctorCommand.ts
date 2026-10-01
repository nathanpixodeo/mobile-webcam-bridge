import { Doctor, check } from '#application/Doctor.ts';
import { toError } from '#domain/errors.ts';
import { createDeviceInfrastructure, createNativeHelper, HOST_VERSION } from '#composition/createBridgeApp.ts';
import type { DoctorCheckResult } from '#ports/NativeHelper.ts';
import { FfmpegLocator } from '#infrastructure/ffmpeg/FfmpegToolchain.ts';
import { ExitCode, type Command, type CommandContext } from '../Command.ts';

/**
 * Checks every prerequisite end to end and explains what to fix. Phone services are optional
 * individually (an Android-only user needs no Apple service and vice versa), so they warn rather
 * than fail; only missing both is a failure.
 */
export class DoctorCommand implements Command {
  readonly name = 'doctor';
  readonly summary = 'Check prerequisites and installation';
  readonly options = {};

  async run(context: CommandContext): Promise<number> {
    const { config, logger, terminal } = context;
    const native = createNativeHelper(context);
    const { usbmux, adb } = createDeviceInfrastructure(context);
    let phoneServices = 0;

    const doctor = new Doctor([
      check('host.node', async () => {
        await Promise.resolve();
        const major = Number(process.versions.node.split('.')[0]);
        return {
          id: 'host.node',
          status: major >= 26 ? 'pass' : 'fail',
          message: `Node ${process.versions.node}, host ${HOST_VERSION}${context.configSource === undefined ? '' : `, config ${context.configSource}`}`,
        };
      }),
      check('host.ffmpeg', async () => ({
        id: 'host.ffmpeg',
        status: 'pass',
        message: await new FfmpegLocator(config.ffmpeg.path, logger).version(),
      })),
      check('iphone.service', async (): Promise<DoctorCheckResult | readonly DoctorCheckResult[]> => {
        if (config.usbmux.tcpHost !== undefined)
          return { id: 'iphone.service', status: 'warn', message: 'TCP development mode' };
        if (usbmux === undefined)
          return { id: 'iphone.service', status: 'warn', message: 'iPhone support disabled (usbmux.enabled)' };
        try {
          const devices = await usbmux.listDevices();
          phoneServices++;
          const results: DoctorCheckResult[] = [
            {
              id: 'iphone.service',
              status: 'pass',
              message: `Apple Mobile Device Service reachable, ${devices.length} iPhone(s) attached`,
            },
          ];
          for (const device of devices) {
            const paired = await usbmux.isPaired(device.id).catch(() => undefined);
            results.push({
              id: 'iphone.trust',
              status: paired === false ? 'fail' : paired === undefined ? 'warn' : 'pass',
              message: `${device.id}: ${paired === false ? 'not trusted — unlock the iPhone and tap Trust' : paired === undefined ? 'trust state unknown' : 'trusted'}`,
            });
          }
          return results;
        } catch (error) {
          return {
            id: 'iphone.service',
            status: 'warn',
            message: `${toError(error).message} (needed only for iPhones: install iTunes from apple.com)`,
          };
        }
      }),
      check('android.adb', async (): Promise<DoctorCheckResult | readonly DoctorCheckResult[]> => {
        if (adb === undefined)
          return { id: 'android.adb', status: 'warn', message: 'Android support disabled (adb.enabled)' };
        try {
          const version = await adb.version();
          const entries = await adb.listDevices();
          phoneServices++;
          return [
            {
              id: 'android.adb',
              status: 'pass',
              message: `ADB server reachable (protocol ${version}), ${entries.length} device(s)`,
            },
            ...entries.map((entry): DoctorCheckResult => ({
              id: 'android.device',
              status: entry.state === 'device' ? 'pass' : entry.state === 'unauthorized' ? 'fail' : 'warn',
              message:
                entry.state === 'unauthorized'
                  ? `${entry.serial}: unauthorized — unlock the phone and accept "Allow USB debugging?"`
                  : `${entry.serial}: ${entry.state}`,
            })),
          ];
        } catch (error) {
          return {
            id: 'android.adb',
            status: 'warn',
            message: `${toError(error).message} (needed only for Android phones: winget install Google.PlatformTools)`,
          };
        }
      }),
      check('phones.service', async () => {
        await Promise.resolve();
        return phoneServices > 0
          ? { id: 'phones.service', status: 'pass', message: `${phoneServices} phone service(s) available` }
          : {
              id: 'phones.service',
              status: 'fail',
              message: 'Neither Apple Mobile Device Service (iPhone) nor the ADB server (Android) is reachable',
            };
      }),
      check('native.helper', async () => {
        if (native === undefined) {
          return {
            id: 'native.helper',
            status: 'fail',
            message: 'bridge-native.exe not found (build native/ and run install)',
          };
        }
        const status = await native.status();
        const checks = await native.doctor();
        return [
          {
            id: 'native.helper',
            status: 'pass' as const,
            message: `${native.executablePath} (installed: ${status.installDir ?? 'no'})`,
          },
          ...checks.map((result) => ({ ...result, id: `native.${result.id}` })),
        ];
      }),
    ]);
    const results = await doctor.run();
    terminal.checks(results);
    const failed = results.filter((result) => result.status === 'fail').length;
    terminal.line();
    terminal.line(failed === 0 ? 'All essential checks passed.' : `${failed} check(s) failed.`);
    return failed === 0 ? ExitCode.Ok : ExitCode.Failure;
  }
}
