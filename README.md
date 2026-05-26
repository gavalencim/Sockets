# CyberSim Multiplayer Protocol

CSMP (CyberSim Multiplayer Protocol) es un protocolo de la capa de aplicación diseñado para soportar la comunicación en tiempo real entre los clientes y el servidor de un juego multijugador de simulación de ciberseguridad. El protocolo opera sobre TCP/IP, utilizando la API de Sockets Berkeley para su implementación.
El sistema modela un centro de datos virtual donde los participantes asumen roles de Atacante o Defensor. El Atacante debe explorar un plano, localizar recursos críticos y ejecutar ataques sobre ellos. El Defensor debe responder a las notificaciones de ataque y mitigar las amenazas antes de que el tiempo límite expire.

