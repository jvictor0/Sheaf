from langgraph.checkpoint.sqlite import SqliteSaver

from sheaf.agent.langgraph_runtime import compile_chat_graph


def main() -> None:
    with SqliteSaver.from_conn_string(":memory:") as saver:
        graph = compile_chat_graph(saver=saver)
        print(graph.get_graph().draw_mermaid())


if __name__ == "__main__":
    main()
