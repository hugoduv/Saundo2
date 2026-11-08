import { spawn } from "node:child_process";
import { createServer } from "node:net";
import { fileURLToPath } from "node:url";

const recognizerScript = fileURLToPath(new URL("./recognize-stop.ps1", import.meta.url));

export function startVoiceServer(host, port, { idleTimeoutMs = 15000, createRecognizer = () => spawn("powershell.exe", [
  "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
  "-File", recognizerScript
], { stdio: ["pipe", "pipe", "pipe"] }) } = {}) {
  let active = false;
  const server = createServer((socket) => {
    if (active) {
      socket.end("ERROR busy\n");
      return;
    }
    active = true;
    const recognizer = createRecognizer();
    let output = "";
    socket.setNoDelay(true);
    socket.setTimeout(idleTimeoutMs);
    socket.on("timeout", () => {
      console.error("[VOICE] Connexion inactive : liberation du microphone.");
      socket.destroy();
    });
    socket.pipe(recognizer.stdin);
    socket.on("error", (error) => console.error("[VOICE] Socket:", error.message));
    recognizer.stdin.on("error", (error) => {
      if (!socket.destroyed && error.code !== "EPIPE" &&
          error.code !== "ERR_STREAM_DESTROYED") {
        console.error("[VOICE] Entree audio:", error.message);
      }
    });
    recognizer.stderr.on("data", (data) => console.error("[VOICE]", data.toString().trim()));
    recognizer.stdout.on("data", (data) => {
      output += data.toString("utf8");
      if (output.length > 1024) {
        console.error("[VOICE] Reponse du moteur trop longue.");
        socket.destroy();
        return;
      }
      let end;
      while ((end = output.indexOf("\n")) !== -1) {
        const line = output.slice(0, end).trim();
        output = output.slice(end + 1);
        if (line === "READY") {
          socket.write("READY\n");
        } else if (line === "PAUSE" || line === "PLAY" || line === "RECOMMENCE") {
          socket.write(`${line}\n`);
        } else {
          console.error("[VOICE] Reponse inattendue du moteur:", line);
          socket.end("ERROR recognizer\n");
        }
      }
    });
    recognizer.on("error", (error) => {
      console.error("[VOICE] Demarrage impossible:", error.message);
      socket.end("ERROR recognizer\n");
    });
    recognizer.on("close", (code) => {
      if (!socket.destroyed) {
        console.error(`[VOICE] Moteur arrete (code ${code}).`);
        socket.end("ERROR recognizer\n");
      }
    });
    socket.on("close", () => {
      active = false;
      recognizer.kill();
    });
  });
  server.on("error", (error) => console.error("[VOICE] Serveur:", error.message));
  server.listen(port, host, () => {
    console.log(`[VOICE] Serveur local en ecoute sur ${host}:${port}.`);
  });
  return server;
}
