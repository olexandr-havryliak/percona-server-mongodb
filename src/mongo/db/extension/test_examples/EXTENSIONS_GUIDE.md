# MongoDB Extensions API — Complete Guide

A single reference for the Extensions API: what it is, how it works, the Fibonacci example (with code), and how to build, use, and verify extensions.

---

## 1. What is the Extensions API?

An **extension** is a compiled shared library (`.so`) that the MongoDB server loads at startup. It lets you add **custom aggregation stages** (e.g. `$fibonacci`) without changing the server binary. Extensions are loaded only on **Linux**.

- **Public API:** A C header (`mongo/db/extension/public/api.h`) that defines the contract between the server and the extension. It is versioned and written in C for a stable ABI.
- **Host:** Server-side code in `mongo/db/extension/host` that loads extensions (e.g. via `dlopen`), checks API version, and calls the extension’s `get_mongodb_extension` and `initialize`.
- **C++ SDK:** Helpers in `mongo/db/extension/sdk` so you can implement extensions in C++ using the Public API. The SDK wraps the C types (e.g. `ExecAggStageSource`, `TestStageDescriptor`, `sdk_log`).

An extension must export the symbol **`get_mongodb_extension`**. The server calls it, agrees on an API version, then calls **`initialize`**, where the extension registers one or more aggregation stage descriptors. Each stage goes through: **Parse → AST → Logical stage → Executable stage**. The executable stage’s `getNext()` (and `open`/`close`) produce or transform documents.

---

## 2. Fibonacci extension — example code

The **`$fibonacci`** stage is a **source stage**: it takes an integer `n` and emits `n` documents `{ index, value }` for the first `n` Fibonacci numbers.

**File:** `src/mongo/db/extension/test_examples/fibonacci.cpp`

### 2.1 Executable stage (runtime)

Produces one document per call to `getNext()` until EOF.

```cpp
class FibonacciExecStage : public sdk::ExecAggStageSource {
public:
    FibonacciExecStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::ExecAggStageSource(stageName), _currentIndex(0) {
        _n = arguments["n"].safeNumberLong();
        if (_n < 0) _n = 0;
    }

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              ::MongoExtensionExecAggStage* execStage) override {
        if (_currentIndex >= _n)
            return extension::ExtensionGetNextResult::eof();

        long long value = _fibonacci(_currentIndex);
        BSONObj doc = BSON("index" << _currentIndex << "value" << value);
        _currentIndex++;
        return extension::ExtensionGetNextResult::advanced(
            extension::ExtensionBSONObj::makeAsByteBuf(doc));
    }

    void open() override { _currentIndex = 0; }
    void reopen() override { _currentIndex = 0; }
    void close() override {}
    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON("n" << _n);
    }

private:
    static long long _fibonacci(long long k) {
        if (k <= 0) return 0;
        if (k == 1) return 1;
        long long a = 0, b = 1;
        for (long long i = 2; i <= k; ++i) {
            long long next = a + b;
            a = b;
            b = next;
        }
        return b;
    }
    long long _n;
    long long _currentIndex;
};
```

### 2.2 Logical stage, AST, parse node, descriptor

The rest wires the exec stage into the pipeline (clone, distributed plan, stage properties, validation):

```cpp
class FibonacciLogicalStage : public sdk::TestLogicalStage<FibonacciExecStage> {
public:
    FibonacciLogicalStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestLogicalStage<FibonacciExecStage>(stageName, arguments) {}
    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<FibonacciLogicalStage>(_name, _arguments);
    }
    boost::optional<extension::sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        extension::sdk::DistributedPlanLogic dpl;
        std::vector<extension::VariantDPLHandle> pipeline;
        pipeline.emplace_back(extension::LogicalAggStageHandle{
            new sdk::ExtensionLogicalAggStageAdapter(clone())});
        dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        return dpl;
    }
};

class FibonacciAstNode : public sdk::TestAstNode<FibonacciLogicalStage> {
public:
    FibonacciAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestAstNode<FibonacciLogicalStage>(stageName, arguments) {}
    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setPosition(extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(extension::MongoExtensionHostTypeRequirementEnum::kRunOnceAnyNode);
        properties.setRequiresInputDocSource(false);
        properties.setAllowedInFacet(false);
        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }
    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<FibonacciAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(Fibonacci);

class FibonacciStageDescriptor : public sdk::TestStageDescriptor<"$fibonacci", FibonacciParseNode> {
public:
    void validate(const BSONObj& arguments) const override {
        sdk_uassert(11999010, "$fibonacci: expected object with integer field 'n'",
                    arguments.hasField("n") && arguments.getField("n").isNumber());
        long long n = arguments["n"].safeNumberLong();
        sdk_uassert(11999011, "$fibonacci: 'n' must be non-negative", n >= 0);
        sdk_uassert(11999012, "$fibonacci: 'n' must be at most 1000", n <= 1000);
    }
};

DEFAULT_EXTENSION(Fibonacci)
REGISTER_EXTENSION(FibonacciExtension)
DEFINE_GET_EXTENSION()
```

**Stage contract:** `{ $fibonacci: { n: <non-negative integer> } }`. Output: `{ index: 0, value: 0 }`, `{ index: 1, value: 1 }`, … (fib(0)=0, fib(1)=1, then standard recurrence).

---

## 3. How to build

**Prerequisites:** Linux, Bazel, repository built at least once.

From the repo root, build the extension with **static linking** and **shared archive** (and set the version):

```bash
bazel build --//bazel/config:linkstatic=True --//bazel/config:shared_archive=True //src/mongo/db/extension/test_examples:fibonacci_mongo_extension_signed_lib --define=MONGO_VERSION=8.3.0
```

The `.so` is produced as a dependency; the path is usually:

```text
bazel-bin/src/mongo/db/extension/test_examples/libfibonacci_mongo_extension.so
```

---

## 4. How to use

### 4.1 Config file

The server loads extensions by **name**. It looks for `<name>.conf` in:

- **Production:** `/etc/mongo/extensions/`
- **Tests (with `enableTestCommands=1`):** `/tmp/mongo/extensions/`

Create the directory and a YAML config (e.g. `/etc/mongo/extensions/fibonacci.conf`):

```yaml
sharedLibraryPath: /absolute/path/to/libfibonacci_mongo_extension.so
```

Use the real path to your built `.so`.

### 4.2 Start mongod

Enable the Extensions API and list the extension name:

```bash
mongod --setParameter featureFlagExtensionsAPI=true --loadExtensions fibonacci
```

For local testing without signature verification:

```bash
mongod --setParameter featureFlagExtensionsAPI=true \
       --setParameter skipExtensionsSignatureVerification=1 \
       --loadExtensions fibonacci
```

If the config is under `/tmp/mongo/extensions`, start with:

```bash
mongod --setParameter featureFlagExtensionsAPI=true \
       --setParameter enableTestCommands=1 \
       --loadExtensions fibonacci
```

### 4.3 Run the stage

Execution requires a **collection name** and the **cursor** option. In **mongosh** (or mongo shell):

```javascript
db.runCommand({
  aggregate: "test",
  pipeline: [{ $fibonacci: { n: 10 } }],
  cursor: {}
})
```

Use any existing or placeholder collection name (e.g. `"test"`); for a source stage like `$fibonacci` the collection is not read, but the aggregate command still requires it. The `cursor: {}` option is required.

---

## 5. How to verify

### 5.1 List loaded extensions

Use **`$listExtensions`** on the **admin** database with a **collectionless** aggregate (`aggregate: 1`). (This is the exception; normal stages like `$fibonacci` require a collection name.)

```javascript
db.getSiblingDB("admin").aggregate([{ $listExtensions: {} }])
```

Expected (with only fibonacci loaded):

```javascript
[{ "extensionName": "fibonacci", "extensionOptions": "{}" }]
```

If nothing is loaded: `[]`. Documents are sorted by `extensionName`.

### 5.2 Run `$fibonacci` and check output

```javascript
db.runCommand({
  aggregate: "test",
  pipeline: [{ $fibonacci: { n: 10 } }],
  cursor: {}
})
```

Expected shape: 10 documents of the form `{ index: <0..9>, value: <fib(i)> }`, e.g.:

```javascript
{ index: 0, value: 0 }
{ index: 1, value: 1 }
{ index: 2, value: 1 }
{ index: 3, value: 2 }
{ index: 4, value: 3 }
{ index: 5, value: 5 }
{ index: 6, value: 8 }
{ index: 7, value: 13 }
{ index: 8, value: 21 }
{ index: 9, value: 34 }
```

### 5.3 Validation checks

- `{ $fibonacci: { n: -1 } }` → error (n must be non-negative).
- `{ $fibonacci: { n: 1001 } }` → error (n ≤ 1000).
- `{ $fibonacci: {} }` or missing `n` → error (expected integer field `n`).

---

## 6. Quick reference

| What              | How |
|-------------------|-----|
| **API header**    | `src/mongo/db/extension/public/api.h` |
| **SDK**           | `src/mongo/db/extension/sdk/` |
| **Build**         | `bazel build --//bazel/config:linkstatic=True --//bazel/config:shared_archive=True //src/mongo/db/extension/test_examples:fibonacci_mongo_extension_signed_lib --define=MONGO_VERSION=8.3.0` |
| **Config dir**    | `/etc/mongo/extensions` or `/tmp/mongo/extensions` |
| **Config content**| `sharedLibraryPath: <absolute path to .so>` |
| **Start mongod**  | `--setParameter featureFlagExtensionsAPI=true --loadExtensions fibonacci` |
| **List extensions** | `db.getSiblingDB("admin").aggregate([{ $listExtensions: {} }])` |
| **Use stage**    | `db.runCommand({ aggregate: "test", pipeline: [{ $fibonacci: { n: 10 } }], cursor: {} })` |
| **Platform**      | Linux only |

---

## 7. Summary

- **Extensions API** = C Public API + host loading + C++ SDK; extensions add custom aggregation stages.
- **Fibonacci example** = one source stage `$fibonacci` with exec/logical/ast/parse/descriptor and validation.
- **Build** = Static build with shared archive and version: `--//bazel/config:linkstatic=True --//bazel/config:shared_archive=True ... --define=MONGO_VERSION=8.3.0`.
- **Use** = create `fibonacci.conf` with `sharedLibraryPath`, start mongod with `featureFlagExtensionsAPI=true` and `loadExtensions fibonacci`, run pipelines with `$fibonacci`.
- **Verify** = `$listExtensions` on admin DB and run `$fibonacci` with small `n` to confirm output shape and values.
