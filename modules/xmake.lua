set_project("flecs_wrapper")
set_version("0.1.0")
set_languages("cxxlatest")

 target("std_module") 
    set_kind("static")
    add_files("std_module.cppm" , {public = true})   
target_end()

 
 
target("Graph")
    set_policy("build.c++.modules", true)
    set_kind("static")
    add_files("graph/**.cppm" , {public = true})  
    add_files("graph/**.cpp")
    add_deps("std_module")
target_end()

 
 

 
 


-- includes("tests")
