import "@typespec/json-schema";
using TypeSpec.JsonSchema;

// JSON conformance exchange only. Oreslang's eq/is and trap effect semantics
// remain defined by the language; this model does not implement them.
@jsonSchema
namespace Oreslang.Assert.V1 {
  model Operand {
    kind: "int" | "bool" | "string";
    value: string;
  }

  model Expected {
    kind: "returned" | "throw" | "none";
    boolean?: boolean;
    message_contains?: string;
  }

  model AssertCase {
    id: string;
    operation: "equal" | "not_equal" | "same" | "true" | "false"
      | "equal_throws" | "not_equal_throws" | "same_throws"
      | "true_throws" | "false_throws" | "fail";
    operands: Operand[];
    trapped: boolean;
    expected: Expected;
  }

  model Suite {
    schema_version: "1.0.0";
    stdlib_module: "std/assert";
    cases: AssertCase[];
  }
}
