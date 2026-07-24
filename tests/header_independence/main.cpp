bool evaluation_header_is_independent();
bool multi_agent_header_is_independent();
bool a2a_header_is_independent();

int main() {
  return evaluation_header_is_independent() &&
         multi_agent_header_is_independent() &&
         a2a_header_is_independent()
           ? 0
           : 1;
}
