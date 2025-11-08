# Sylvester Wired

Sylvester Wired é um firmware para o módulo WT32-ETH01 que procura controladores industriais em uma LAN cabeada, publica os
dados em MQTT e oferece um console web pensado para técnicos. O dispositivo pode ser instalado em um painel, alimentado com 5 V
e fará a varredura da rede /24 local em busca de CLPs enquanto transmite logs em tempo real para o painel e para o broker MQTT.

> Procura detalhes voltados a desenvolvedores? Consulte [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md).

## Hardware em Destaque
- **Controlador:** WT32-ETH01 (ESP32 com PHY LAN8720)
- **Alimentação:** 5 V CC via o conector do módulo (≈300 mA de pico durante operação Wi-Fi)
- **Rede:** Ethernet 10/100 para uplink e ponto de acesso Wi-Fi local para configuração
- **Interfaces de Campo:** Broker MQTT, painel HTTP e túnel TCP persistente para o CLP descoberto

## Checklist de Primeira Inicialização
1. **Grave o firmware** usando sua ferramenta preferida para ESP32 (Arduino IDE, `esptool.py` etc.).
2. **Conecte o cabo Ethernet** à rede da planta e alimente o módulo com uma fonte regulada de 5 V.
3. **Aguarde o link:** o dispositivo espera um IP cabeado antes de iniciar o MQTT ou a varredura. Os logs seriais (115200 baud)
repetem o que aparecerá no painel.
4. **Entre no Wi-Fi técnico:** SSID `sylvester-<mac sem dois-pontos>` com senha padrão `12345678`.
5. **Abra o painel:** acesse `http://192.168.4.1/` para acompanhar a descoberta e os logs em tempo real.

## Operação Diária
### Painel Web
- **Monitorar status:** `/` exibe o estado da Ethernet, o último comando MQTT, a última requisição/resposta ao CLP e o buffer de
logs com rolagem automática.
- **Configurar endereço:** `/config` permite alternar entre DHCP e IP fixo para a porta cabeada. Os campos de IP estático são
preenchidos com os valores salvos anteriormente.
- **API de logs:** `/logs` retorna JSON, útil para ferramentas externas que precisem ingerir os registros sem a interface web.

### Integração MQTT
Todos os tópicos MQTT são prefixados por `sylvester/<mac-do-dispositivo>/`, onde `<mac-do-dispositivo>` é o MAC em minúsculas
sem dois-pontos.

| Tópico | Direção | Uso |
|--------|---------|-----|
| `log` | Publicação | Fluxo de logs em tempo real, igual ao painel. |
| `response/<crc32>` | Publicação | Respostas do CLP identificadas pelo CRC32 do comando original. |
| `command` | Assinatura | Ações de gerenciamento do dispositivo (veja abaixo). |
| `request` | Assinatura | Agendador de consultas ao CLP; formato `COMANDO` ou `COMANDO|intervaloMs|flagEnvioSempre`. |

#### Comandos Suportados em `command`
- `boot` — Reinicia o dispositivo.
- `factoryreset` — Apaga configurações salvas (portas, senha Wi-Fi, perfil cabeado) e reinicia.
- `scan` — Força uma nova varredura de rede em busca de CLPs.
- `port <p1> [p2 ...]` — Substitui a lista de portas TCP (1–10 valores). A varredura reinicia automaticamente.
- `ipconfig dhcp` — Retorna a interface cabeada para DHCP.
- `ipconfig fixed <ip> <mask> <gateway> <dns>` — Define um endereço cabeado estático.
- `wifipassword <novasenha>` — Atualiza a senha do ponto de acesso (8–63 caracteres). O AP reinicia na hora.
- `ota <firmware.bin> <firmware.md5>` — Executa atualização OTA com verificação de integridade.

#### Agendamento de Requisições ao CLP em `request`
Envie o comando bruto do CLP como carga útil. Metadados opcionais permitem automação:
- `COMANDO|60000|1` → envia a cada 60 s e sempre publica as respostas.
- `COMANDO|0|` → envia uma vez; o slot do agendador é liberado ao terminar.
- Se `flagEnvioSempre` for omitida, a resposta só é republicada quando houver mudança.

### Descoberta e Túnel até o CLP
- O scanner percorre a faixa `/24` derivada da máscara do IP cabeado usando até oito workers em paralelo.
- Para cada IP candidato, testa a lista de portas configurada e envia `(&V)` para confirmar o banner do CLP.
- Quando um CLP responde, o dispositivo mantém uma conexão TCP persistente e encaminha os `request` do MQTT por ela.
- O último IP/porta conhecido do CLP é salvo para reconexão rápida após quedas de energia.

## Manutenção e Solução de Problemas
- **Acompanhe os logs:** console serial, tópico MQTT `log` e painel exibem as mesmas mensagens.
- **Problemas de Ethernet:** se DHCP/IP fixo estiver incorreto e impedir a conexão, envie `ipconfig dhcp` via MQTT ou use a
página Wi-Fi de configuração para voltar ao padrão.
- **Segurança em OTA:** o dispositivo valida o MD5 antes de aplicar o firmware. Informe as URLs do `.bin` e do `.md5` correspondente.
- **Restauração de fábrica:** limpa senha Wi-Fi, lista de portas, ajustes cabeados e agendamentos. Utilize ao realocar o
hardware.

## Localização
Uma tradução deste README para português do Brasil está disponível neste arquivo. O original em inglês continua em
[README.md](README.md).
