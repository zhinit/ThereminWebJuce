import { useState, useRef } from "react";
import "./App.css";

function App() {
  const [isPlaying, setIsPlaying] = useState(false);
  const [isReady, setIsReady] = useState(false);
  const audioContextRef = useRef<AudioContext | null>(null);
  const workletNodeRef = useRef<AudioWorkletNode | null>(null);

  const initAudio = async () => {
    if (audioContextRef.current) return;

    const ctx = new AudioContext();

    // Fetch the Emscripten glue code in main thread
    const response = await fetch("/audio-engine.js");
    const scriptCode = await response.text();

    await ctx.audioWorklet.addModule("/dsp-processor.js");

    const node = new AudioWorkletNode(ctx, "dsp-processor");
    node.connect(ctx.destination);

    node.port.onmessage = (e) => {
      if (e.data.type === "ready") {
        setIsReady(true);
      }
    };

    // Send the script code to the worklet
    node.port.postMessage({ type: "init", scriptCode });

    audioContextRef.current = ctx;
    workletNodeRef.current = node;
  };

  const togglePlay = async () => {
    if (!audioContextRef.current) {
      await initAudio();
    }

    if (audioContextRef.current?.state === "suspended") {
      await audioContextRef.current.resume();
    }

    if (isPlaying) {
      workletNodeRef.current?.port.postMessage({ type: "stop" });
    } else {
      workletNodeRef.current?.port.postMessage({ type: "play" });
    }
    setIsPlaying(!isPlaying);
  };

  return (
    <div>
      <h1>C++ JUCE WASM Saw Wave with Filter POC</h1>
      <button
        onClick={togglePlay}
        style={{ padding: "1rem 2rem", fontSize: "1.2rem" }}
      >
        {isPlaying ? "Stop" : "Play 110Hz"}
      </button>
      {!isReady && audioContextRef.current && <p>Loading WASM...</p>}
    </div>
  );
}

export default App;
