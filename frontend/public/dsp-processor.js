class DSPProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.engine = null;
    this.module = null;
    this.heapBufferLeft = null;
    this.heapBufferRight = null;
    this.port.onmessage = (e) => this.handleMessage(e.data);
  }

  async handleMessage(data) {
    // initialization - receive script from main thread
    if (data.type === "init") {
      // Execute the Emscripten glue code sent from main thread
      const fn = new Function(data.scriptCode + "; return createAudioEngine;");
      const createAudioEngine = fn();
      const module = await createAudioEngine();
      this.engine = new module.Sampler();
      this.engine.prepare(sampleRate);
      this.module = module;
      this.port.postMessage({ type: "ready" });
    }
    if (data.type === "loadSample") {
      const samplePtr = this.module._malloc(data.samples.length * 4);
      this.module.HEAPF32.set(data.samples, samplePtr / 4);
      this.engine?.loadSample(samplePtr, data.samples.length);
    }
    // when ui sends play msg
    if (data.type === "play") {
      this.engine?.trigger();
    }
    // when ui send loop msg
    if (data.type === "loop") {
      this.engine?.setLooping(data.inLoop);
    }
    // // when ui sends stop msg
    // if (data.type === "stop") {
    //   this.engine?.setPlaying(false);
    // }
  }

  process(inputs, outputs, parameters) {
    // if engine or module missing, return but keep processor alive
    if (!this.engine || !this.module) return true;

    // get stereo output channels
    const leftOutput = outputs[0][0];
    const rightOutput = outputs[0][1];
    const numSamples = leftOutput.length;

    // allocate heap buffers if needed
    if (!this.heapBufferLeft || this.heapBufferLeft.length < numSamples) {
      if (this.heapBufferLeft) this.module._free(this.heapBufferLeft);
      if (this.heapBufferRight) this.module._free(this.heapBufferRight);
      this.heapBufferLeft = this.module._malloc(numSamples * 4);
      this.heapBufferRight = this.module._malloc(numSamples * 4);
    }

    // call wasm process function which puts result in wasm heap memory
    this.engine.process(this.heapBufferLeft, this.heapBufferRight, numSamples);

    // pull audio from wasm heap memory so it can be played in browser
    const wasmLeft = new Float32Array(
      this.module.HEAPF32.buffer,
      this.heapBufferLeft,
      numSamples,
    );
    const wasmRight = new Float32Array(
      this.module.HEAPF32.buffer,
      this.heapBufferRight,
      numSamples,
    );
    leftOutput.set(wasmLeft);
    rightOutput.set(wasmRight);

    // call me again when the next block of samples is needed
    return true;
  }
}

registerProcessor("dsp-processor", DSPProcessor);
