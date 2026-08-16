
 
module; // global fragment

export module graph; // 模块名字 

import graph.impl; 
export namespace graph {
    using ::graph::WeightBox;
    using ::graph::DirectedGraph;
    using ::graph::CSR;
    using ::graph::CSC; 
    using id_type = ::graph::id_type;
}  