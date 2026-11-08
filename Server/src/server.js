import { createReadStream } from "node:fs";
import { promises as fs } from "node:fs";
import { createHash, randomUUID } from "node:crypto";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { isAbsolute, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { startVoiceServer } from "./voice-server.js";

const serverDirectory = resolve(fileURLToPath(new URL("..", import.meta.url)));
const dataFile = join(serverDirectory, "data", "tracks.json");
const audioDirectory = resolve(serverDirectory, "audio");
const cacheDirectory = join(serverDirectory, "cache");
const port = Number.parseInt(process.env.PORT ?? "8080", 10);
const host = process.env.HOST ?? "0.0.0.0";
const ffmpeg = process.env.FFMPEG_PATH ?? "ffmpeg";
const conversions = new Map();

const outputFormat = { format: "wav", sampleRate: 16000, channels: 1, bitsPerSample: 16 };

function normalizeUid(uid) {
  return uid.replace(/[^0-9a-f]/gi, "").toUpperCase();
}

function sendJson(response, statusCode, payload) {
  const body = JSON.stringify(payload);
  response.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(body),
    "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": "*"
  });
  response.end(body);
}

function sendError(response, statusCode, message) {
  sendJson(response, statusCode, { error: message });
}

async function loadTracks() {
  const contents = await fs.readFile(dataFile, "utf8");
  const tracks = JSON.parse(contents);
  if (!Array.isArray(tracks)) {
    throw new Error("tracks.json must contain an array");
  }
  return tracks;
}

function audioPathFor(fileName) {
  if (typeof fileName !== "string" || fileName.length === 0) {
    return null;
  }

  const filePath = resolve(audioDirectory, fileName);
  const relativePath = relative(audioDirectory, filePath);
  if (!relativePath || relativePath === ".." ||
      relativePath.startsWith(`..${sep}`) || isAbsolute(relativePath)) {
    return null;
  }
  return filePath;
}

const formats = {
  wav: { extension: "wav", contentType: "audio/wav",
    arguments: ["-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", "-f", "wav"] },
  mp3: { extension: "mp3", contentType: "audio/mpeg",
    arguments: ["-ar", "32000", "-ac", "1", "-c:a", "libmp3lame", "-b:a", "64k",
      "-write_xing", "0", "-id3v2_version", "0", "-write_id3v1", "0", "-f", "mp3"] }
};

function runFfmpeg(source, destination, format) {
  return new Promise((resolveConversion, rejectConversion) => {
    const process = spawn(ffmpeg, [
      "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
      "-i", source, "-map", "0:a:0", "-vn",
      ...format.arguments, destination
    ], { stdio: ["ignore", "ignore", "pipe"] });
    let errors = "";
    process.stderr.on("data", (chunk) => { errors += chunk.toString(); });
    process.on("error", rejectConversion);
    process.on("close", (code) => {
      if (code === 0) {
        resolveConversion();
      } else {
        rejectConversion(new Error(`FFmpeg failed (code ${code}): ${errors.trim()}`));
      }
    });
  });
}

async function convertedAudioPath(source, sourceStats, formatName) {
  const format = formats[formatName];
  const key = createHash("sha256")
    .update(JSON.stringify([formatName, format.arguments, source, sourceStats.size, sourceStats.mtimeMs, sourceStats.ctimeMs]))
    .digest("hex");
  const output = join(cacheDirectory, `${key}.${format.extension}`);
  if (!conversions.has(key)) {
    const conversion = (async () => {
      await fs.mkdir(cacheDirectory, { recursive: true });
      try {
        await fs.access(output);
        return output;
      } catch (error) {
        if (error.code !== "ENOENT") throw error;
      }
      const temporary = join(cacheDirectory, `${key}-${randomUUID()}.tmp`);
      try {
        await runFfmpeg(source, temporary, format);
        await fs.rename(temporary, output);
      } finally {
        await fs.rm(temporary, { force: true });
      }
      return output;
    })();
    conversions.set(key, conversion);
    conversion.then(
      () => conversions.delete(key),
      () => conversions.delete(key)
    );
  }
  return conversions.get(key);
}

async function serveAudio(request, response, fileName, formatName = "wav") {
  const sourcePath = audioPathFor(fileName);
  if (!sourcePath) {
    sendError(response, 400, "Invalid audio file");
    return;
  }

  let fileStats;
  try {
    fileStats = await fs.stat(sourcePath);
  } catch (error) {
    if (error.code === "ENOENT") {
      sendError(response, 404, "Audio file not found");
      return;
    }
    throw error;
  }

  if (!fileStats.isFile()) {
    sendError(response, 404, "Audio file not found");
    return;
  }

  const filePath = await convertedAudioPath(sourcePath, fileStats, formatName);
  fileStats = await fs.stat(filePath);
  const range = request.headers.range;
  let start = 0;
  let end = fileStats.size - 1;
  let statusCode = 200;

  if (range) {
    const match = /^bytes=(\d*)-(\d*)$/.exec(range);
    if (!match || (!match[1] && !match[2])) {
      response.writeHead(416, { "Content-Range": `bytes */${fileStats.size}` });
      response.end();
      return;
    }

    if (match[1] === "") {
      const suffixLength = Number(match[2]);
      start = Math.max(fileStats.size - suffixLength, 0);
    } else {
      start = Number(match[1]);
      if (match[2] !== "") {
        end = Number(match[2]);
      }
    }

    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) ||
        start > end || start >= fileStats.size) {
      response.writeHead(416, {
        "Content-Range": `bytes */${fileStats.size}`,
        "Access-Control-Allow-Origin": "*"
      });
      response.end();
      return;
    }
    end = Math.min(end, fileStats.size - 1);
    statusCode = 206;
  }

  const headers = {
    "Accept-Ranges": "bytes",
    "Content-Length": fileStats.size === 0 ? 0 : end - start + 1,
    "Content-Type": formats[formatName].contentType,
    "Access-Control-Allow-Origin": "*"
  };
  if (statusCode === 206) {
    headers["Content-Range"] = `bytes ${start}-${end}/${fileStats.size}`;
  }
  response.writeHead(statusCode, headers);

  if (request.method === "HEAD" || fileStats.size === 0) {
    response.end();
    return;
  }

  const stream = createReadStream(filePath, { start, end });
  stream.on("error", (error) => {
    console.error("Error reading audio file:", error);
    response.destroy(error);
  });
  response.on("close", () => stream.destroy());
  stream.pipe(response);
}

async function handleRequest(request, response) {
  if (request.method === "OPTIONS") {
    response.writeHead(204, {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type, Range"
    });
    response.end();
    return;
  }

  const requestUrl = new URL(request.url, `http://${request.headers.host ?? "localhost"}`);
  const pathParts = requestUrl.pathname.split("/").filter(Boolean);

  if (pathParts[0] === "health" && request.method === "GET") {
    sendJson(response, 200, { status: "ok" });
    return;
  }

  if (pathParts.length === 3 && pathParts[0] === "api" &&
      (pathParts[1] === "tracks" || pathParts[1] === "stream" || pathParts[1] === "stream-mp3") &&
      request.method === "GET") {
    const uid = normalizeUid(pathParts[2] ?? "");
    if (!/^[0-9A-F]{8,32}$/.test(uid)) {
      sendError(response, 400, "UID must contain between 8 and 32 hexadecimal characters");
      return;
    }

    const tracks = await loadTracks();
    const track = tracks.find((candidate) =>
      candidate.enabled !== false && normalizeUid(candidate.uid) === uid
    );
    if (!track) {
      sendError(response, 404, "No enabled track is associated with this UID");
      return;
    }

    if (pathParts[1] === "stream" || pathParts[1] === "stream-mp3") {
      await serveAudio(request, response, track.file, pathParts[1] === "stream-mp3" ? "mp3" : "wav");
      return;
    }

    sendJson(response, 200, {
      uid,
      title: track.title,
      ...outputFormat,
      audioUrl: new URL(`/audio/${encodeURIComponent(track.file)}`, requestUrl).toString()
    });
    return;
  }

  if (pathParts[0] === "audio" && (request.method === "GET" || request.method === "HEAD")) {
    let fileName;
    try {
      fileName = decodeURIComponent(pathParts.slice(1).join("/"));
    } catch {
      sendError(response, 400, "Invalid audio path");
      return;
    }
    await serveAudio(request, response, fileName);
    return;
  }

  sendError(response, 404, "Route not found");
}

const server = createServer((request, response) => {
  handleRequest(request, response).catch((error) => {
    console.error(error);
    if (!response.headersSent) {
      sendError(response, 500, "Internal server error");
    } else {
      response.destroy();
    }
  });
});

server.listen(port, host, () => {
  console.log(`Saundo2 server listening on http://${host}:${port}`);
});

if (process.platform === "win32" && port < 65535) {
  const admission = { active: false };
  startVoiceServer(host, port + 1, { admission });
  if (port < 65534) startVoiceServer(host, port + 2, { encoding: "u8", admission });
  else console.error("[VOICE] Port vocal U8 indisponible : port HTTP trop eleve.");
} else {
  console.error("[VOICE] Reconnaissance indisponible : Windows et un port libre sont necessaires.");
}
