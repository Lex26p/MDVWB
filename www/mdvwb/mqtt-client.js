const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

function encodeString(value) {
  const bytes = textEncoder.encode(String(value));
  if (bytes.length > 65535) {
    throw new Error("MQTT string is too long");
  }
  return Uint8Array.of(bytes.length >> 8, bytes.length & 0xff, ...bytes);
}

function encodeRemainingLength(length) {
  const output = [];
  let value = length;
  do {
    let digit = value % 128;
    value = Math.floor(value / 128);
    if (value > 0) {
      digit |= 0x80;
    }
    output.push(digit);
  } while (value > 0);
  return Uint8Array.from(output);
}

function makePacket(header, body) {
  const remaining = encodeRemainingLength(body.length);
  const packet = new Uint8Array(1 + remaining.length + body.length);
  packet[0] = header;
  packet.set(remaining, 1);
  packet.set(body, 1 + remaining.length);
  return packet;
}

function concatenate(parts) {
  const length = parts.reduce((sum, item) => sum + item.length, 0);
  const output = new Uint8Array(length);
  let offset = 0;
  parts.forEach((part) => {
    output.set(part, offset);
    offset += part.length;
  });
  return output;
}

function decodeRemainingLength(bytes, start) {
  let multiplier = 1;
  let value = 0;
  let index = start;
  let digit = 0;

  do {
    if (index >= bytes.length || multiplier > 128 * 128 * 128) {
      return null;
    }
    digit = bytes[index++];
    value += (digit & 0x7f) * multiplier;
    multiplier *= 128;
  } while ((digit & 0x80) !== 0);

  return { value, bytesUsed: index - start };
}

function readString(bytes, offset) {
  if (offset + 2 > bytes.length) {
    throw new Error("Malformed MQTT string");
  }
  const length = (bytes[offset] << 8) | bytes[offset + 1];
  const start = offset + 2;
  const end = start + length;
  if (end > bytes.length) {
    throw new Error("Malformed MQTT string length");
  }
  return { value: textDecoder.decode(bytes.subarray(start, end)), next: end };
}

export class TinyMqttClient {
  constructor(options = {}) {
    this.url = options.url;
    this.clientId = options.clientId || `mdvwb-web-${Math.random().toString(16).slice(2, 12)}`;
    this.keepAliveSeconds = options.keepAliveSeconds || 30;
    this.reconnectDelayMs = options.reconnectDelayMs || 2000;
    this.username = options.username || "";
    this.password = options.password || "";
    this.socket = null;
    this.connected = false;
    this.closedByUser = false;
    this.packetId = 1;
    this.incoming = new Uint8Array(0);
    this.keepAliveTimer = null;
    this.reconnectTimer = null;
    this.subscriptions = new Set();
    this.onConnect = () => {};
    this.onDisconnect = () => {};
    this.onMessage = () => {};
    this.onError = () => {};
  }

  connect() {
    this.closedByUser = false;
    this.clearReconnectTimer();

    try {
      this.socket = new WebSocket(this.url, ["mqtt"]);
      this.socket.binaryType = "arraybuffer";
      this.socket.addEventListener("open", () => this.sendConnect());
      this.socket.addEventListener("message", (event) => this.handleSocketMessage(event));
      this.socket.addEventListener("close", () => this.handleSocketClose());
      this.socket.addEventListener("error", () => this.onError(new Error("MQTT WebSocket connection error")));
    } catch (error) {
      this.onError(error);
      this.scheduleReconnect();
    }
  }

  disconnect() {
    this.closedByUser = true;
    this.clearReconnectTimer();
    this.stopKeepAlive();
    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(makePacket(0xe0, new Uint8Array(0)));
    }
    if (this.socket) {
      this.socket.close();
    }
  }

  subscribe(topic) {
    this.subscriptions.add(topic);
    if (!this.connected) {
      return;
    }

    const packetId = this.nextPacketId();
    const body = concatenate([
      Uint8Array.of(packetId >> 8, packetId & 0xff),
      encodeString(topic),
      Uint8Array.of(0),
    ]);
    this.socket.send(makePacket(0x82, body));
  }

  publish(topic, payload, options = {}) {
    if (!this.connected) {
      throw new Error("MQTT is not connected");
    }
    const retain = options.retain === true;
    const header = 0x30 | (retain ? 0x01 : 0x00);
    const body = concatenate([encodeString(topic), textEncoder.encode(String(payload))]);
    this.socket.send(makePacket(header, body));
  }

  sendConnect() {
    let flags = 0x02;
    const payloadParts = [encodeString(this.clientId)];

    if (this.username) {
      flags |= 0x80;
      payloadParts.push(encodeString(this.username));
    }
    if (this.password) {
      flags |= 0x40;
      payloadParts.push(encodeString(this.password));
    }

    const variableHeader = concatenate([
      encodeString("MQTT"),
      Uint8Array.of(4, flags, this.keepAliveSeconds >> 8, this.keepAliveSeconds & 0xff),
    ]);
    const body = concatenate([variableHeader, ...payloadParts]);
    this.socket.send(makePacket(0x10, body));
  }

  handleSocketMessage(event) {
    const chunk = new Uint8Array(event.data);
    this.incoming = concatenate([this.incoming, chunk]);
    this.parsePackets();
  }

  parsePackets() {
    let offset = 0;

    while (offset + 2 <= this.incoming.length) {
      const remaining = decodeRemainingLength(this.incoming, offset + 1);
      if (!remaining) {
        break;
      }

      const bodyStart = offset + 1 + remaining.bytesUsed;
      const packetEnd = bodyStart + remaining.value;
      if (packetEnd > this.incoming.length) {
        break;
      }

      const header = this.incoming[offset];
      const body = this.incoming.subarray(bodyStart, packetEnd);
      this.handlePacket(header, body);
      offset = packetEnd;
    }

    if (offset > 0) {
      this.incoming = this.incoming.slice(offset);
    }
  }

  handlePacket(header, body) {
    const type = header >> 4;

    if (type === 2) {
      if (body.length < 2 || body[1] !== 0) {
        const code = body.length >= 2 ? body[1] : -1;
        this.onError(new Error(`MQTT connection rejected: ${code}`));
        this.socket.close();
        return;
      }
      this.connected = true;
      this.startKeepAlive();
      [...this.subscriptions].forEach((topic) => this.subscribe(topic));
      this.onConnect();
      return;
    }

    if (type === 3) {
      try {
        const topic = readString(body, 0);
        const qos = (header >> 1) & 0x03;
        let payloadOffset = topic.next;
        if (qos > 0) {
          payloadOffset += 2;
        }
        const payload = textDecoder.decode(body.subarray(payloadOffset));
        this.onMessage(topic.value, payload, { retained: (header & 0x01) !== 0, qos });
      } catch (error) {
        this.onError(error);
      }
    }
  }

  handleSocketClose() {
    const wasConnected = this.connected;
    this.connected = false;
    this.stopKeepAlive();
    this.socket = null;
    this.incoming = new Uint8Array(0);
    this.onDisconnect(wasConnected);
    if (!this.closedByUser) {
      this.scheduleReconnect();
    }
  }

  scheduleReconnect() {
    if (this.reconnectTimer || this.closedByUser) {
      return;
    }
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, this.reconnectDelayMs);
  }

  clearReconnectTimer() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  startKeepAlive() {
    this.stopKeepAlive();
    this.keepAliveTimer = setInterval(() => {
      if (this.socket && this.socket.readyState === WebSocket.OPEN) {
        this.socket.send(makePacket(0xc0, new Uint8Array(0)));
      }
    }, Math.max(5000, Math.floor(this.keepAliveSeconds * 500)));
  }

  stopKeepAlive() {
    if (this.keepAliveTimer) {
      clearInterval(this.keepAliveTimer);
      this.keepAliveTimer = null;
    }
  }

  nextPacketId() {
    const current = this.packetId;
    this.packetId = this.packetId >= 65535 ? 1 : this.packetId + 1;
    return current;
  }
}
