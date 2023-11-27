# ESP32: Interfaceamento com microfones digitais

Código fonte referente ao tutorial: https://embarcados.com.br/esp32-interfaceamento-com-microfones-digitais/

## Configuração adicional

Acesse o menuconfig no item `Configurando interface I2S` para definir configurações do tipo de aquisição (I2S ou PDM) e do IP e porta do servidor para receber os dados de áudio (as configurações de Wi-Fi são realizadas no item `Example Connection Configuration`).

```
(Top) → Configurando interface I2S
    Protocolo de aquisição (USE_I2S_ACQUISITION)  --->
(127.0.0.1) IP do servidor
(9999) Porta do servidor
```