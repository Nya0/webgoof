document.addEventListener('DOMContentLoaded', function() {
    const button = document.getElementById('testBtn');
    const message = document.getElementById('message');
    
    const responses = [
        'connection successful',
        'server responding',
        'latency: 12ms',
        'all systems operational',
        'ping: 200 OK',
        'status: running',
        'uptime: 99.9%',
        'response code: 200',
        'health check passed',
        'connection stable'
    ];
    
    let clickCount = 0;
    
    button.addEventListener('click', function() {
        // Add a glitch effect to button
        button.style.transform = 'translateX(2px)';
        setTimeout(() => {
            button.style.transform = 'translateX(-2px)';
        }, 50);
        setTimeout(() => {
            button.style.transform = 'translateX(0)';
        }, 100);
        
        // Typing effect for message
        const response = responses[clickCount % responses.length];
        message.textContent = '';
        let i = 0;
        
        const typeWriter = setInterval(() => {
            if (i < response.length) {
                message.textContent += response.charAt(i);
                i++;
            } else {
                clearInterval(typeWriter);
            }
        }, 30);
        
        clickCount++;
    });
});