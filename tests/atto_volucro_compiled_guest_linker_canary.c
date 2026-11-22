// SOURCE-FREE toolchain canary. This is C, NOT an AttoLang compiled actor.
// Exercises the scalar lifecycle ABI and Wasm32 native linker shape only.
static long long accumulator;
void oreslang_wasm_actor_init(void) { accumulator = 0; }
long long oreslang_wasm_actor_on_message(long long delta) {
  accumulator += delta;
  return accumulator;
}
void oreslang_wasm_actor_teardown(void) { accumulator = 0; }
