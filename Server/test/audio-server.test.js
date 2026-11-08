import { strict as assert } from "node:assert";
import { spawn, spawnSync } from "node:child_process";
import { once } from "node:events";
import { promises as fs } from "node:fs";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

const ffmpeg = process.env.FFMPEG_PATH ?? "ffmpeg";
const available = spawnSync(ffmpeg, ["-version"], { stdio: "ignore" }).status === 0;

async function unusedPort() {
  const server = createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

test("MP3 route preserves WAV API and produces complete 32 kHz mono frames", {
  skip: !available && "FFmpeg unavailable",
  timeout: 30000
}, async () => {
  const dir = await fs.mkdtemp(join(tmpdir(), "saundo-audio-test-"));
  let child;
  try {
    for (const name of ["src", "data", "audio"]) await fs.mkdir(join(dir, name));
    for (const name of ["server.js", "voice-server.js"]) {
      await fs.copyFile(new URL(`../src/${name}`, import.meta.url), join(dir, "src", name));
    }
    await fs.writeFile(join(dir, "package.json"), '{"type":"module"}');
    await fs.writeFile(join(dir, "data", "tracks.json"),
      JSON.stringify([{ uid: "12345678", title: "test", file: "tone.wav" }]));
    const tone = spawnSync(ffmpeg, ["-hide_banner", "-loglevel", "error", "-f", "lavfi",
      "-i", "sine=frequency=440:duration=1", "-y", join(dir, "audio", "tone.wav")]);
    assert.equal(tone.status, 0, tone.stderr.toString());

    const port = await unusedPort();
    child = spawn(process.execPath, [join(dir, "src", "server.js")], {
      env: { ...process.env, HOST: "127.0.0.1", PORT: String(port), FFMPEG_PATH: ffmpeg },
      stdio: ["ignore", "pipe", "pipe"]
    });
    await Promise.race([
      once(child.stdout, "data"),
      once(child, "exit").then(([code]) => { throw new Error(`server exited: ${code}`); })
    ]);
    const root = `http://127.0.0.1:${port}`;
    const mp3Response = await fetch(`${root}/api/stream-mp3/12345678`);
    assert.equal(mp3Response.status, 200);
    assert.equal(mp3Response.headers.get("content-type"), "audio/mpeg");
    const mp3 = Buffer.from(await mp3Response.arrayBuffer());
    assert.equal(Number(mp3Response.headers.get("content-length")), mp3.length);
    let offset = 0, frames = 0;
    while (offset < mp3.length) {
      assert.ok(offset + 4 <= mp3.length);
      assert.equal(mp3[offset], 0xff);
      assert.equal(mp3[offset + 1] & 0xfe, 0xfa);
      assert.equal(mp3[offset + 2] & 0xfc, 0x58);
      assert.equal(mp3[offset + 3] & 0xc0, 0xc0);
      offset += 288 + ((mp3[offset + 2] >> 1) & 1);
      frames++;
    }
    assert.equal(offset, mp3.length);
    assert.ok(frames >= 28);
    const decoded = spawnSync(ffmpeg, ["-hide_banner", "-loglevel", "error", "-f", "mp3",
      "-i", "pipe:0", "-f", "s16le", "pipe:1"], { input: mp3, maxBuffer: 1024 * 1024 });
    assert.equal(decoded.status, 0, decoded.stderr.toString());
    assert.equal(decoded.stdout.length, frames * 1152 * 2);

    const wavResponse = await fetch(`${root}/api/stream/12345678`);
    assert.equal(wavResponse.status, 200);
    assert.equal(wavResponse.headers.get("content-type"), "audio/wav");
    const wav = Buffer.from(await wavResponse.arrayBuffer());
    assert.equal(wav.toString("ascii", 0, 4), "RIFF");
    const audio = await fetch(`${root}/audio/tone.wav`);
    assert.deepEqual(Buffer.from(await audio.arrayBuffer()), wav);
    const range = await fetch(`${root}/api/stream-mp3/12345678`, { headers: { Range: "bytes=10-29" } });
    assert.equal(range.status, 206);
    assert.deepEqual(Buffer.from(await range.arrayBuffer()), mp3.subarray(10, 30));
    assert.equal((await fetch(`${root}/api/stream-mp3/87654321`)).status, 404);
    const cache = await fs.readdir(join(dir, "cache"));
    assert.equal(cache.filter((name) => name.endsWith(".mp3")).length, 1);
    assert.equal(cache.filter((name) => name.endsWith(".wav")).length, 1);
  } finally {
    if (child && child.exitCode === null) {
      child.kill();
      await once(child, "exit");
    }
    await fs.rm(dir, { recursive: true, force: true });
  }
});
