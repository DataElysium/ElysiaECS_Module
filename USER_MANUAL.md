# Elysia ECS v0.1 - 用户手册

## 1. 宣言：为什么是 Elysia？

在 ECS 的世界里，开发者往往在“极致性能”与“易用性”之间做痛苦的抉择。Elysia 不同，我们选择了“全都要”。

**Elysia 的哲学：性能是灵魂，魔法是阶梯。**

我们不同于其他框架的核心支柱：

*   **层级是扁平的 (Flat Hierarchy)**：不同于传统 OOP 臃肿深沉的继承树，Elysia 的数据和逻辑天生扁平。SoA 的存储确保了 Cache 的极致命中，而基于波次的调度确保了逻辑的清爽。扁平，意味着预测性，意味着在现代处理器上无可匹敌的速度。
*   **C++20 是模块的 (Modular C++20)**：这不仅是一个技术标签，而是一场工业级革命。Elysia 彻底摒弃了陈旧的头文件包含模式，全程采用 C++20 Modules 重构。通过分区 (`:base`, `:executor`)、模块化接口与后期解析，我们展示了如何在复杂的工业级项目中优雅地利用 BMI (Built Module Interface) 实现闪电般的编译与坚固的封装。
*   **逻辑与物理分离**：你描述的是逻辑流，我们编译的是物理波次。
*   **零歧义同步**：不再猜测命令何时生效，显式屏障结合编译器折叠，让并行开发如履平地。

如果 Linus 看到这些代码，也许会皱眉于某些模板魔法，但当他看到 **10,000 个系统在 500us 内分发完毕**，看到 **100,000 个实体的流水线仅需 0.2ms** 时，他会明白：**这里的每一行代码，都是为了在现代分布式架构上换取绝对的物理效率。**

---

## 2. 快速入门

### 定义数据
```cpp
struct Pos { float x, y; };
struct Vel { float dx, dy; };
```

### 定义系统
```cpp
import elysia; // 一行搞定所有

// 像写作文一样定义流水线
scheduler.chain("Loading", Sync, "Computing", Sync, "Finalizing");

// 使用 Phase 引导器添加系统
auto computing = scheduler.phase("Computing");

computing.add("Move", [](Pos& p, const Vel& v) {
    p.x += v.dx;
    p.y += v.dy;
});
```

### 发动引擎
```cpp
// 创建执行器并运行
auto exec = ForkUnionExecutor::build_from(scheduler);
exec->run(&world);
```

---

## 3. 核心 API 参考

### 调度神器：`chain`
`chain` 是 Elysia 的逻辑骨架。它可以混合使用字符串名、`Sync` 标记和系统描述符。
*   `scheduler.chain("A", Sync, "B")`：保证 A 的修改对 B 绝对可见。

### 同步锚点：`Sync`
不要手动去管理 `ApplyDeferred` 的位置。只需在 `chain` 中插入 `schedule::Sync`，编译器会自动为你寻找最优的物理同步点并合并多余的开销。

### 执行器选择
*   **SerialExecutor**：调试之王。每一步都自动提交，所见即所得。
*   **TaskflowExecutor**：通用的 DAG 执行器，适合高度非对称的任务图。
*   **ForkUnionExecutor**：Elysia 的超空间引擎。专为海量数据并行和极低延迟调度设计。

---

## 4. 结语
Elysia ECS 还在成长。v0.1 代表了我们对“科学调度”这一命题的初步答卷。我们相信，通过对 C++ 底层的极致压榨和对上层 API 的反复抛光，Elysia 终将成为高性能模拟与游戏开发的坚实基石。

**时代在前进，代码在歌唱。**
