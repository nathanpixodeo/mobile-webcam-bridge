import assert from 'node:assert/strict';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { nextEvent } from '#domain/events.ts';
import type { AdbDevice } from '#ports/DeviceWatcher.ts';
import { silentLogger } from '#ports/Logger.ts';
import { AdbClient } from '#infrastructure/adb/AdbClient.ts';
import { AdbDeviceWatcher } from '#infrastructure/adb/AdbDeviceWatcher.ts';
import {
  encodeAdbRequest,
  failureToError,
  parseDeviceList,
  toAdbDevice,
  toPendingDevice,
} from '#infrastructure/adb/AdbProtocol.ts';
import { AdbServerLauncher, locateAdb } from '#infrastructure/adb/AdbServerLauncher.ts';
import { AdbTunnelFactory } from '#infrastructure/adb/AdbTunnelFactory.ts';
import { SystemClock } from '#infrastructure/system/SystemClock.ts';
import { FakeAdbServer, type AdbReply } from '#test/fakes/FakeAdbServer.ts';
import { waitFor } from '#test/support/waitFor.ts';

const READY = 'R5CT1234ABC           device usb:1-1 product:e3qxeea model:SM_S918B device:e3q transport_id:3';
const UNAUTHORIZED = '0123456789ABCDEF      unauthorized usb:1-2 transport_id:4';
const LONG_LIST = `${READY}\n${UNAUTHORIZED}\n`;

const PHONE: AdbDevice = {
  transport: 'adb',
  platform: 'android',
  id: 'R5CT1234ABC',
  serial: 'R5CT1234ABC',
  link: 'usb',
  model: undefined,
};

/** Records `start()` calls instead of spawning adb.exe. */
class CountingLauncher extends AdbServerLauncher {
  starts = 0;

  constructor() {
    super({ adbPath: 'adb.exe', logger: silentLogger });
  }

  override start(): Promise<boolean> {
    this.starts++;
    return Promise.resolve(false);
  }
}

describe('encodeAdbRequest', () => {
  it('prefixes the service with its length as four hex digits', () => {
    assert.deepEqual(encodeAdbRequest('host:version'), Buffer.from('000chost:version'));
  });

  it('counts UTF-8 bytes, not characters', () => {
    assert.deepEqual(encodeAdbRequest('host:é'), Buffer.from('0007host:é'));
  });

  it('rejects a service that does not fit the four-digit prefix', () => {
    assert.throws(() => encodeAdbRequest('x'.repeat(0x10000)), RangeError);
    assert.equal(encodeAdbRequest('x'.repeat(0xffff)).subarray(0, 4).toString(), 'ffff');
  });
});

describe('parseDeviceList', () => {
  it('parses the short track-devices format', () => {
    const entries = parseDeviceList('R5CT1234ABC\tdevice\n0123456789ABCDEF\tunauthorized\n');
    assert.deepEqual(
      entries.map((entry) => [entry.serial, entry.state]),
      [
        ['R5CT1234ABC', 'device'],
        ['0123456789ABCDEF', 'unauthorized'],
      ],
    );
    assert.equal(entries[0]?.properties.size, 0);
  });

  it('parses the long format into properties split at the first colon', () => {
    const entry = parseDeviceList(READY)[0]!;
    assert.equal(entry.serial, 'R5CT1234ABC');
    assert.equal(entry.state, 'device');
    assert.equal(entry.properties.get('usb'), '1-1');
    assert.equal(entry.properties.get('model'), 'SM_S918B');
    assert.equal(entry.properties.get('transport_id'), '3');
  });

  it('ignores blank lines and carriage returns', () => {
    assert.deepEqual(parseDeviceList(''), []);
    assert.deepEqual(
      parseDeviceList('\r\nR5CT1234ABC\tdevice\r\n\r\n').map((entry) => entry.serial),
      ['R5CT1234ABC'],
    );
  });
});

describe('device mapping', () => {
  it('maps a ready phone, showing the model without underscores', () => {
    const [entry] = parseDeviceList(READY);
    const device = toAdbDevice(entry!);
    assert.deepEqual(device, { ...PHONE, model: 'SM S918B' });
    assert.equal(toPendingDevice(entry!), undefined);
  });

  it('leaves the model undefined when the server reports none', () => {
    assert.equal(toAdbDevice(parseDeviceList('R5CT1234ABC\tdevice')[0]!)?.model, undefined);
  });

  it('keeps an unauthorized phone pending', () => {
    const entry = parseDeviceList(UNAUTHORIZED)[0]!;
    assert.equal(toAdbDevice(entry), undefined);
    assert.deepEqual(toPendingDevice(entry), { id: '0123456789ABCDEF', platform: 'android', state: 'unauthorized' });
  });

  it('treats wireless debugging serials as network devices', () => {
    for (const serial of ['192.168.1.20:5555', 'adb-R5CT1234ABC-x1y2z3._adb-tls-connect._tcp']) {
      assert.equal(toAdbDevice(parseDeviceList(`${serial}\tdevice`)[0]!)?.link, 'network', serial);
    }
  });
});

describe('failureToError', () => {
  it('explains an unauthorized phone and keeps the server message', () => {
    const error = failureToError('host:transport:R5CT1234ABC', 'device unauthorized.');
    assert.equal(error.code, 'DEVICE_UNAVAILABLE');
    assert.match(error.message, /Allow USB debugging/);
    assert.match(error.message, /device unauthorized\./);
  });

  it('maps offline and unknown phones to DEVICE_UNAVAILABLE', () => {
    assert.equal(failureToError('host:transport:R5CT1234ABC', 'device offline').code, 'DEVICE_UNAVAILABLE');
    assert.equal(
      failureToError('host:transport:R5CT1234ABC', "device 'R5CT1234ABC' not found").code,
      'DEVICE_UNAVAILABLE',
    );
    assert.equal(failureToError('host:transport-any', 'no devices/emulators found').code, 'DEVICE_UNAVAILABLE');
  });

  it('maps a refused tcp: service to APP_NOT_REACHABLE and any other refusal to ADB_PROTOCOL', () => {
    assert.equal(failureToError('tcp:27100', 'closed').code, 'APP_NOT_REACHABLE');
    assert.equal(failureToError('host:track-devices-l', 'unknown host service').code, 'ADB_PROTOCOL');
  });
});

describe('AdbClient against a fake ADB server', { timeout: 15_000 }, () => {
  it('reads the server version', async () => {
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      reply.block('0029');
    });
    assert.equal(await new AdbClient(adb.address).version(), 41);
    assert.deepEqual(adb.requests, ['host:version']);
  });

  it('reassembles a reply that arrives in pieces', async () => {
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.raw(Buffer.from('OKAY00'));
      setTimeout(() => {
        reply.raw(Buffer.from('04002'));
      }, 10);
      setTimeout(() => {
        reply.raw(Buffer.from('9'));
      }, 20);
    });
    assert.equal(await new AdbClient(adb.address).version(), 41);
  });

  it('lists devices in the long format', async () => {
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      reply.block(LONG_LIST);
    });
    const entries = await new AdbClient(adb.address).listDevices();
    assert.deepEqual(adb.requests, ['host:devices-l']);
    assert.deepEqual(
      entries.map((entry) => entry.state),
      ['device', 'unauthorized'],
    );
  });

  it('hands over the tunnel including bytes that arrived with the second OKAY', async () => {
    await using adb = await FakeAdbServer.start((service, reply, socket) => {
      if (service === 'host:transport:R5CT1234ABC') {
        reply.okay();
      } else if (service === 'tcp:27100') {
        reply.raw(Buffer.concat([Buffer.from('OKAY'), Buffer.from('early-bytes')]));
        setTimeout(() => {
          socket.write('late-bytes');
        }, 20);
      } else {
        reply.fail(`unexpected service ${service}`);
      }
    });
    const factory = new AdbTunnelFactory(new AdbClient(adb.address));
    await using tunnel = await factory.open(PHONE, 27_100, new AbortController().signal);
    const received = await new Promise<string>((resolve) => {
      let text = '';
      tunnel.stream.on('data', (chunk: Buffer) => {
        text += chunk.toString();
        if (text.includes('late-bytes')) resolve(text);
      });
      tunnel.stream.resume();
    });
    assert.equal(received, 'early-byteslate-bytes');
    assert.equal(tunnel.label, 'adb:R5CT1234ABC:27100');
    assert.deepEqual(adb.requests, ['host:transport:R5CT1234ABC', 'tcp:27100']);
  });

  it('refuses to open a tunnel for a device of another transport', async () => {
    await using adb = await FakeAdbServer.start(() => undefined);
    const factory = new AdbTunnelFactory(new AdbClient(adb.address));
    await assert.rejects(
      factory.open(
        { transport: 'tcp', platform: 'unknown', id: 't', link: 'network', model: undefined },
        27_100,
        new AbortController().signal,
      ),
      /cannot reach a tcp device/,
    );
    assert.deepEqual(adb.requests, []);
  });

  it('reports an unauthorized phone as DEVICE_UNAVAILABLE', async () => {
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.fail('device unauthorized.');
    });
    await assert.rejects(new AdbClient(adb.address).connect('R5CT1234ABC', 27_100), {
      name: 'TransportError',
      code: 'DEVICE_UNAVAILABLE',
      message: /Allow USB debugging/,
    });
  });

  it('reports a connection closed before OKAY on tcp: as APP_NOT_REACHABLE', async () => {
    await using adb = await FakeAdbServer.start((service, reply) => {
      if (service.startsWith('host:transport:')) reply.okay();
      else reply.close();
    });
    await assert.rejects(new AdbClient(adb.address).connect('R5CT1234ABC', 27_100), { code: 'APP_NOT_REACHABLE' });
  });

  it('reports a FAIL on tcp: as APP_NOT_REACHABLE', async () => {
    await using adb = await FakeAdbServer.start((service, reply) => {
      if (service.startsWith('host:transport:')) reply.okay();
      else reply.fail('closed');
    });
    await assert.rejects(new AdbClient(adb.address).connect('R5CT1234ABC', 27_100), {
      code: 'APP_NOT_REACHABLE',
      message: /closed/,
    });
  });

  it('reports a missing ADB server as ADB_UNAVAILABLE', async () => {
    let port: number;
    {
      await using adb = await FakeAdbServer.start(() => undefined);
      port = adb.address.port;
    }
    await assert.rejects(new AdbClient({ host: '127.0.0.1', port }).version(), {
      name: 'TransportError',
      code: 'ADB_UNAVAILABLE',
    });
  });

  it('falls back to track-devices when the server rejects the long format', async () => {
    await using adb = await FakeAdbServer.start((service, reply) => {
      if (service === 'host:track-devices-l') {
        reply.fail('unknown host service');
        return;
      }
      reply.okay();
      reply.block('R5CT1234ABC\tdevice\n');
    });
    await using connection = await new AdbClient(adb.address).trackDevices();
    const entries = parseDeviceList(await connection.readLengthPrefixed());
    assert.deepEqual(
      entries.map((entry) => entry.serial),
      ['R5CT1234ABC'],
    );
    assert.deepEqual(adb.requests, ['host:track-devices-l', 'host:track-devices']);
  });

  it('rejects with the abort reason while the server stays silent', async () => {
    await using adb = await FakeAdbServer.start(() => undefined);
    const controller = new AbortController();
    const reading = new AdbClient(adb.address).version(controller.signal);
    setTimeout(() => {
      controller.abort();
    }, 20);
    await assert.rejects(reading, { name: 'AbortError' });
  });
});

describe('AdbDeviceWatcher against a fake ADB server', { timeout: 15_000 }, () => {
  it('attaches ready phones, keeps unauthorized ones pending and detaches when the list empties', async () => {
    const tracking = Promise.withResolvers<AdbReply>();
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      // The wireless phone must be ignored: network devices are opt-in.
      reply.block(`${LONG_LIST}192.168.1.20:5555\tdevice\n`);
      tracking.resolve(reply);
    });
    await using watcher = new AdbDeviceWatcher({
      client: new AdbClient(adb.address),
      clock: new SystemClock(),
      logger: silentLogger,
    });
    const attached: string[] = [];
    using _attached = watcher.events.on('attached', (device) => {
      attached.push(device.id);
    });

    await watcher.start();

    assert.deepEqual(attached, ['R5CT1234ABC']);
    assert.deepEqual(
      watcher.devices().map((device) => device.id),
      ['R5CT1234ABC'],
    );
    assert.deepEqual(watcher.pending(), [{ id: '0123456789ABCDEF', platform: 'android', state: 'unauthorized' }]);

    const detached = nextEvent(watcher.events, 'detached', AbortSignal.timeout(5000));
    (await tracking.promise).block('');
    assert.deepEqual(await detached, ['R5CT1234ABC']);
    assert.deepEqual(watcher.devices(), []);
    assert.deepEqual(watcher.pending(), []);
  });

  it('includes wireless phones when asked to', async () => {
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      reply.block('192.168.1.20:5555\tdevice\n');
    });
    await using watcher = new AdbDeviceWatcher({
      client: new AdbClient(adb.address),
      clock: new SystemClock(),
      logger: silentLogger,
      includeNetworkDevices: true,
    });
    await watcher.start();
    assert.deepEqual(
      watcher.devices().map((device) => [device.id, device.link]),
      [['192.168.1.20:5555', 'network']],
    );
  });

  it('starts the ADB server once per outage, then reports it unavailable', async () => {
    let port: number;
    {
      await using adb = await FakeAdbServer.start(() => undefined);
      port = adb.address.port;
    }
    const launcher = new CountingLauncher();
    await using watcher = new AdbDeviceWatcher({
      client: new AdbClient({ host: '127.0.0.1', port }),
      clock: new SystemClock(),
      logger: silentLogger,
      launcher,
    });
    const unavailable = nextEvent(watcher.events, 'unavailable', AbortSignal.timeout(10_000));

    await watcher.start();

    const [error] = await unavailable;
    assert.equal(error.code, 'ADB_UNAVAILABLE');
    assert.equal(launcher.starts, 1);
  });

  it('reports the server unavailable and forgets every phone when it drops the connection', async () => {
    const tracking = Promise.withResolvers<AdbReply>();
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      reply.block(LONG_LIST);
      tracking.resolve(reply);
    });
    await using watcher = new AdbDeviceWatcher({
      client: new AdbClient(adb.address),
      clock: new SystemClock(),
      logger: silentLogger,
    });
    const unavailable = nextEvent(watcher.events, 'unavailable', AbortSignal.timeout(5000));
    const detached = nextEvent(watcher.events, 'detached', AbortSignal.timeout(5000));
    await watcher.start();
    assert.equal(watcher.devices().length, 1);
    assert.equal(watcher.pending().length, 1);

    (await tracking.promise).close();

    const [error] = await unavailable;
    assert.equal(error.code, 'ADB_UNAVAILABLE');
    assert.deepEqual(await detached, ['R5CT1234ABC']);
    assert.deepEqual(watcher.devices(), []);
    assert.deepEqual(watcher.pending(), []);
  });

  it('retries at once after starting the server and keeps the phones attached meanwhile', async () => {
    const connections: AdbReply[] = [];
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      reply.block(`${READY}\n`);
      connections.push(reply);
    });
    const launcher = new CountingLauncher();
    await using watcher = new AdbDeviceWatcher({
      client: new AdbClient(adb.address),
      clock: new SystemClock(),
      logger: silentLogger,
      launcher,
    });
    const changes: string[] = [];
    using _attached = watcher.events.on('attached', (device) => {
      changes.push(`attached ${device.id}`);
    });
    using _detached = watcher.events.on('detached', (id) => {
      changes.push(`detached ${id}`);
    });
    await watcher.start();

    connections[0]?.close();
    await waitFor(() => connections.length === 2, 'the watcher to reconnect');

    assert.equal(launcher.starts, 1);
    assert.deepEqual(changes, ['attached R5CT1234ABC']);
    assert.equal(watcher.devices().length, 1);
  });

  it('warns once per unauthorized phone, however many lists repeat it', async () => {
    const tracking = Promise.withResolvers<AdbReply>();
    await using adb = await FakeAdbServer.start((_service, reply) => {
      reply.okay();
      reply.block(`${UNAUTHORIZED}\n`);
      tracking.resolve(reply);
    });
    const warnings: string[] = [];
    await using watcher = new AdbDeviceWatcher({
      client: new AdbClient(adb.address),
      clock: new SystemClock(),
      logger: {
        ...silentLogger,
        warn: (message) => {
          warnings.push(message);
        },
      },
    });
    await watcher.start();
    assert.deepEqual(warnings, ['Android device needs authorization']);
    const reply = await tracking.promise;

    // The same list again, then the phone gets authorized: the repeat must not warn again.
    const attached = nextEvent(watcher.events, 'attached', AbortSignal.timeout(5000));
    reply.block(`${UNAUTHORIZED}\n`);
    reply.block('0123456789ABCDEF\tdevice\n');
    await attached;
    assert.equal(warnings.length, 1);

    // Losing the authorization is a new prompt to accept.
    const detached = nextEvent(watcher.events, 'detached', AbortSignal.timeout(5000));
    reply.block(`${UNAUTHORIZED}\n`);
    await detached;
    assert.equal(warnings.length, 2);
  });
});

describe('AdbServerLauncher', () => {
  it('resolves false instead of throwing when adb.exe cannot be started', async () => {
    const launcher = new AdbServerLauncher({
      adbPath: join(tmpdir(), 'bridge-webcam-no-such-dir', 'adb.exe'),
      logger: silentLogger,
    });
    assert.equal(await launcher.start(), false);
  });

  it('locates the configured executable when it exists', () => {
    assert.equal(locateAdb(process.execPath), process.execPath);
  });
});
