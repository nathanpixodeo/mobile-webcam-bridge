import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { TransportError } from '#domain/errors.ts';
import { UsbmuxClient, parseDeviceEntry } from '#infrastructure/usbmux/UsbmuxClient.ts';
import {
  decodeUsbmuxHeader,
  decodeUsbmuxPayload,
  encodeUsbmuxMessage,
  toUsbmuxPortNumber,
  USBMUX_HEADER_SIZE,
} from '#infrastructure/usbmux/UsbmuxCodec.ts';
import { UsbmuxTunnelFactory } from '#infrastructure/usbmux/UsbmuxTunnelFactory.ts';
import { FakeUsbmuxd } from '#test/fakes/FakeUsbmuxd.ts';
import { hex, usbmuxVectors } from '#test/support/vectors.ts';

const vectors = usbmuxVectors();

describe('usbmux codec against golden vectors', () => {
  it('encodes the port in network byte order', () => {
    assert.equal(toUsbmuxPortNumber(vectors.portNumber.port), vectors.portNumber.field);
  });

  it('encodes a little-endian header', () => {
    const packet = encodeUsbmuxMessage(vectors.header.tag, { MessageType: 'ListDevices' });
    const header = decodeUsbmuxHeader(packet.subarray(0, USBMUX_HEADER_SIZE));
    assert.equal(header.length, packet.length);
    assert.equal(header.tag, vectors.header.tag);
    const sized = Buffer.from(vectors.header.hex, 'hex');
    assert.equal(decodeUsbmuxHeader(sized).length, vectors.header.payloadLength + USBMUX_HEADER_SIZE);
  });

  it('parses an Attached message into a device', () => {
    const bytes = hex(vectors.attached.hex);
    const header = decodeUsbmuxHeader(bytes.subarray(0, USBMUX_HEADER_SIZE));
    const message = decodeUsbmuxPayload(bytes.subarray(USBMUX_HEADER_SIZE, header.length));
    assert.deepEqual(parseDeviceEntry(message), {
      muxDeviceId: vectors.attached.device.deviceId,
      transport: 'usbmux',
      platform: 'ios',
      id: vectors.attached.device.udid,
      link: 'usb',
      model: undefined,
      productId: vectors.attached.device.productId,
    });
  });

  it('rejects absurd lengths', () => {
    const header = Buffer.alloc(16);
    header.writeUInt32LE(8, 0);
    assert.throws(() => decodeUsbmuxHeader(header), TransportError);
  });
});

describe('UsbmuxClient against a fake usbmuxd', () => {
  it('lists devices', async () => {
    const attached = decodeUsbmuxPayload(hex(vectors.attached.hex).subarray(USBMUX_HEADER_SIZE));
    await using usbmuxd = await FakeUsbmuxd.start((request, reply) => {
      assert.equal(request['MessageType'], 'ListDevices');
      reply({ DeviceList: [attached] });
    });
    const devices = await new UsbmuxClient(usbmuxd.address).listDevices();
    assert.equal(devices.length, 1);
    assert.equal(devices[0]?.id, vectors.attached.device.udid);
  });

  it('hands over the tunnel including bytes that arrived with the Connect result', async () => {
    const early = Buffer.from('early-bytes');
    await using usbmuxd = await FakeUsbmuxd.start((request, reply, socket) => {
      assert.equal(request['MessageType'], 'Connect');
      assert.equal(request['PortNumber'], vectors.portNumber.field);
      reply({ MessageType: 'Result', Number: 0 }, early);
      setTimeout(() => socket.write('late-bytes'), 20);
    });
    const factory = new UsbmuxTunnelFactory(new UsbmuxClient(usbmuxd.address));
    await using tunnel = await factory.open(
      {
        transport: 'usbmux',
        platform: 'ios',
        id: 'u',
        muxDeviceId: 3,
        link: 'usb',
        model: undefined,
        productId: undefined,
      },
      vectors.portNumber.port,
      new AbortController().signal,
    );
    const received = await new Promise<string>((resolve) => {
      let text = '';
      tunnel.stream.on('data', (chunk: Buffer) => {
        text += chunk.toString();
        if (text.includes('late-bytes')) resolve(text);
      });
      tunnel.stream.resume();
    });
    assert.equal(received, 'early-byteslate-bytes');
  });

  it('maps ConnectionRefused to APP_NOT_REACHABLE', async () => {
    await using usbmuxd = await FakeUsbmuxd.start((_request, reply) => {
      reply({ MessageType: 'Result', Number: 3 });
    });
    await assert.rejects(
      new UsbmuxClient(usbmuxd.address).connect(3, 27100),
      (error: unknown) => error instanceof TransportError && error.code === 'APP_NOT_REACHABLE',
    );
  });

  it('reports usbmuxd being down as USBMUX_UNAVAILABLE', async () => {
    let port: number;
    {
      await using usbmuxd = await FakeUsbmuxd.start(() => undefined);
      port = usbmuxd.address.port;
    }
    await assert.rejects(
      new UsbmuxClient({ host: '127.0.0.1', port }).listDevices(),
      (error: unknown) => error instanceof TransportError && error.code === 'USBMUX_UNAVAILABLE',
    );
  });
});
