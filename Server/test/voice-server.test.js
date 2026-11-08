import { strict as assert } from "node:assert";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { connect } from "node:net";
import { test } from "node:test";
import { startVoiceServer } from "../src/voice-server.js";

function fakeRecognizer() {
  return spawn(process.execPath, ["-e", `
    process.stdout.write("READY\\n");
    let count = 0;
    process.stdin.on("data", () => process.stdout.write(["PAUSE\\n", "PLAY\\n", "RECOMMENCE\\n"][count++]));
    process.stdin.resume();
  `], { stdio: ["pipe", "pipe", "pipe"] });
}

async function openServer() {
  const server = startVoiceServer("127.0.0.1", 0, { createRecognizer: fakeRecognizer });
  await once(server, "listening");
  return server;
}

function openClient(server) {
  return connect(server.address().port, "127.0.0.1");
}

function readUntil(socket, text) {
  return new Promise((resolve, reject) => {
    let response = "";
    socket.on("data", function onData(data) {
      response += data.toString();
      if (response.includes(text)) {
        socket.off("data", onData);
        resolve(response);
      }
    });
    socket.once("error", reject);
    socket.once("close", () => reject(new Error(`Socket closed before ${text}`)));
  });
}

test("forwards multiple commands in the same microphone session", { timeout: 5000 }, async () => {
  const server = await openServer();
  const socket = openClient(server);
  try {
    assert.match(await readUntil(socket, "READY\n"), /READY\n/);
    for (const command of ["PAUSE", "PLAY", "RECOMMENCE"]) {
      const response = readUntil(socket, `${command}\n`);
      socket.write(Buffer.alloc(1024));
      assert.match(await response, new RegExp(`${command}\n`));
    }
  } finally {
    socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test("rejects a second microphone while one is connected", { timeout: 5000 }, async () => {
  const server = await openServer();
  const first = openClient(server);
  let second;
  try {
    await readUntil(first, "READY\n");
    second = openClient(server);
    assert.match(await readUntil(second, "ERROR busy\n"), /ERROR busy\n/);
  } finally {
    first.destroy();
    if (second) second.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test("releases an inactive microphone connection for the next client", { timeout: 5000 }, async () => {
  const server = startVoiceServer("127.0.0.1", 0, {
    idleTimeoutMs: 200,
    createRecognizer: fakeRecognizer
  });
  await once(server, "listening");
  const first = openClient(server);
  let second;
  try {
    await readUntil(first, "READY\n");
    await once(first, "close");
    second = openClient(server);
    assert.match(await readUntil(second, "READY\n"), /READY\n/);
  } finally {
    first.destroy();
    if (second) second.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test("rejects the removed stop command", { timeout: 5000 }, async () => {
  const server = startVoiceServer("127.0.0.1", 0, {
    createRecognizer: () => spawn(process.execPath, ["-e", `
      process.stdout.write("STOP\\n");
      process.stdin.resume();
    `], { stdio: ["pipe", "pipe", "pipe"] })
  });
  await once(server, "listening");
  const socket = openClient(server);
  try {
    assert.match(await readUntil(socket, "ERROR recognizer\n"), /ERROR recognizer\n/);
  } finally {
    socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

  test("converts fragmented unsigned 8-bit microphone samples to signed 16-bit PCM", { timeout: 5000 }, async () => {
    const server = startVoiceServer("127.0.0.1", 0, {
      encoding: "u8",
      createRecognizer: () => spawn(process.execPath, ["-e", `
        process.stdout.write("READY\\n");
        let data = Buffer.alloc(0);
        process.stdin.on("data", chunk => {
          data = Buffer.concat([data, chunk]);
          if (data.length >= 8) {
            process.stdout.write(data.subarray(0, 8).toString("hex") === "008000ff0000007f" ? "PAUSE\\n" : "BAD\\n");
          }
        });
      `], { stdio: ["pipe", "pipe", "pipe"] })
    });
    await once(server, "listening");
    const socket = openClient(server);
    try {
      await readUntil(socket, "READY\n");
      const response = readUntil(socket, "PAUSE\n");
      socket.write(Buffer.from([0, 127]));
      socket.write(Buffer.from([128, 255]));
      assert.match(await response, /PAUSE/);
    } finally {
      socket.destroy();
      await new Promise((resolve) => server.close(resolve));
    }
  });

  test("legacy and compressed ports share microphone exclusivity", { timeout: 5000 }, async () => {
    const admission = { active: false };
    const legacy = startVoiceServer("127.0.0.1", 0, { admission, createRecognizer: fakeRecognizer });
    const compressed = startVoiceServer("127.0.0.1", 0, { admission, encoding: "u8", createRecognizer: fakeRecognizer });
    await Promise.all([once(legacy, "listening"), once(compressed, "listening")]);
    const first = openClient(legacy);
    let second;
    try {
      await readUntil(first, "READY\n");
      second = openClient(compressed);
      assert.match(await readUntil(second, "ERROR busy\n"), /ERROR busy/);
    } finally {
      first.destroy();
      if (second) second.destroy();
      await Promise.all([new Promise((resolve) => legacy.close(resolve)),
        new Promise((resolve) => compressed.close(resolve))]);
    }
  });
